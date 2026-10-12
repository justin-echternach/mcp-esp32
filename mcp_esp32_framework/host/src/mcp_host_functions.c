#include "mcp_host_functions.h"
#include "sdkconfig.h"
#include "mcp_memory.h"
#include "mcp_config_loader.h"
#include "mcp_session_manager.h"
#include "mcp_pthread_pool.h"
#include "mcp_bluetooth_host.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2s.h"
#include "mcp_config.h"
#include "mcp_http_server.h"
#include <math.h>

static const char *TAG = "MCP_HOST";

// WASM memory alignment size
#define MALLOC_ALIGN_SIZE 8

// Thread-local storage for current request context (used by header functions)
static _Thread_local mcp_request_t *tls_current_request = NULL;

// MIN macro if not defined
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// External global reference (from main.c)
extern mcp_config_t g_mcp_config;

// ========== WAMR Runtime Memory Allocators (ESP32-specific) ==========

static void *wamr_malloc(unsigned int size)
{
    void *ptr;
#ifdef CONFIG_SPIRAM
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    uint32_t caps = MALLOC_CAP_8BIT;
#endif
    ptr = heap_caps_aligned_alloc(MALLOC_ALIGN_SIZE, size, caps);
    ESP_LOGV(TAG, "malloc ptr=%p size=%u", ptr, size);
    return ptr;
}

static void wamr_free(void *ptr)
{
    ESP_LOGV(TAG, "free ptr=%p", ptr);
    heap_caps_free(ptr);
}

static void *wamr_realloc(void *ptr, unsigned int size)
{
    void *new_ptr;
    new_ptr = wamr_malloc(size);
    if (new_ptr)
    {
        if (ptr)
        {
            size_t n = heap_caps_get_allocated_size(ptr);
            size_t m = MIN(size, n);
            memcpy(new_ptr, ptr, m);
            wamr_free(ptr);
        }
    }
    ESP_LOGV(TAG, "realloc ptr=%p size=%u new_ptr=%p", ptr, size, new_ptr);
    return new_ptr;
}

// ========== WASM Memory Management Functions ==========

uint32_t mcp_native_malloc(wasm_exec_env_t exec_env, uint32_t size)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

    if (size == 0 || size > 64 * 1024)
    { // Reasonable size limit
        ESP_LOGE(TAG, "Invalid malloc size: %u", size);
        return 0;
    }

    uint32_t wasm_addr = wasm_runtime_module_malloc(module_inst, size, NULL);
    if (wasm_addr == 0)
    {
        ESP_LOGE(TAG, "WASM malloc failed for size %u", size);
    }

    return wasm_addr;
}

// uint32_t mcp_native_malloc(wasm_exec_env_t exec_env, uint32_t size) {
//     wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

//     if (size == 0 || size > 64 * 1024) { // Reasonable size limit
//         ESP_LOGE(TAG, "Invalid malloc size: %u", size);
//         return 0;
//     }

//     void * native_addr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
//     if (native_addr == 0) {
//         ESP_LOGE(TAG, "WASM malloc failed for size %u", size);
//     }

//     uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, native_addr);

//     return wasm_addr;
// }

void mcp_native_free(wasm_exec_env_t exec_env, uint32_t ptr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

    if (ptr == 0)
    {
        return; // Nothing to free
    }

    wasm_runtime_module_free(module_inst, ptr);
}

// void mcp_native_free(wasm_exec_env_t exec_env, uint32_t ptr) {
//     wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

//     if (ptr == 0) {
//         return; // Nothing to free
//     }

//     void * native_addr = wasm_runtime_addr_app_to_native(module_inst, ptr);

//     heap_caps_free(native_addr);
// }

// ========== String Functions ==========

int32_t mcp_native_strlen(wasm_exec_env_t exec_env, const char *s)
{
    if (!s)
    {
        return 0;
    }
    return (int32_t)strlen(s);
}

int32_t mcp_native_strcmp(wasm_exec_env_t exec_env, const char *s1, const char *s2)
{
    if (!s1 || !s2)
    {
        return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    }
    return strcmp(s1, s2);
}

int32_t mcp_native_strcpy(wasm_exec_env_t exec_env, char *dest, const char *src)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

    if (!dest || !src)
    {
        return 0;
    }

    strcpy(dest, src);

    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, dest);
    return wasm_addr;
}

int32_t mcp_native_strcat(wasm_exec_env_t exec_env, char *dest, const char *src)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

    if (!dest || !src)
    {
        return 0;
    }

    // "$$" only proves dest is a valid string where it is now - make sure the
    // appended bytes (plus NUL) also land inside the module's linear memory
    size_t dest_len = strlen(dest);
    size_t src_len = strlen(src);
    if (!wasm_runtime_validate_native_addr(module_inst, dest + dest_len, (uint64_t)src_len + 1))
    {
        ESP_LOGE(TAG, "strcat: write of %u bytes would leave module memory", (unsigned)(src_len + 1));
        return 0;
    }

    memcpy(dest + dest_len, src, src_len + 1);

    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, dest);
    return wasm_addr;
}

int32_t mcp_native_strncpy(wasm_exec_env_t exec_env, char *dest, const char *src, int32_t n)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

    if (!dest || !src || n <= 0)
    {
        return 0;
    }

    strncpy(dest, src, n);

    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, dest);
    return wasm_addr;
}

int32_t mcp_native_strstr(wasm_exec_env_t exec_env, const char *haystack, const char *needle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

    if (!haystack || !needle)
    {
        return 0;
    }

    char *result = strstr(haystack, needle);
    if (!result)
    {
        return 0;
    }

    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, result);
    return wasm_addr;
}

int32_t mcp_native_atoi(wasm_exec_env_t exec_env, const char *str)
{
    if (!str)
    {
        return 0;
    }

    return atoi(str);
}

// ========== Memory Functions ==========

int32_t mcp_native_memcpy(wasm_exec_env_t exec_env, void *dest, const void *src, int32_t n)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

    if (!dest || !src || n <= 0)
    {
        return 0;
    }

    memcpy(dest, src, n);

    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, dest);
    return wasm_addr;
}

int32_t mcp_native_memcmp(wasm_exec_env_t exec_env, const void *s1, const void *s2, int32_t n)
{
    // Using automatic pointer conversion (**i) - no validation needed
    return memcmp(s1, s2, n);
}

int32_t mcp_native_strncmp(wasm_exec_env_t exec_env, const char *s1, const char *s2, int32_t n)
{
    // Using automatic string conversion ($$) - no validation needed
    return strncmp(s1, s2, n);
}

// ========== Logging Functions ==========

int32_t mcp_native_log_info(wasm_exec_env_t exec_env, const char *tag, const char *message)
{
    if (!tag || !message)
    {
        ESP_LOGE(TAG, "LOG_INFO: Invalid parameters - tag=%p, message=%p", tag, message);
        return -1;
    }

    // Limit string lengths for safety
    size_t tag_len = strnlen(tag, 64);
    size_t msg_len = strnlen(message, 512);

    if (tag_len == 0 || msg_len == 0)
    {
        ESP_LOGW(TAG, "LOG_INFO: Empty string detected - tag_len=%zu, msg_len=%zu", tag_len, msg_len);
        return -1;
    }

    ESP_LOGI(tag, "[WASM] %s", message);
    return 0;
}

int32_t mcp_native_log_error(wasm_exec_env_t exec_env, const char *tag, const char *message)
{
    if (!tag || !message)
    {
        ESP_LOGE(TAG, "LOG_ERROR: Invalid parameters - tag=%p, message=%p", tag, message);
        return -1;
    }

    // Limit string lengths for safety
    size_t tag_len = strnlen(tag, 64);
    size_t msg_len = strnlen(message, 512);

    if (tag_len == 0 || msg_len == 0)
    {
        ESP_LOGW(TAG, "LOG_ERROR: Empty string detected - tag_len=%zu, msg_len=%zu", tag_len, msg_len);
        return -1;
    }

    ESP_LOGE(tag, "[WASM] %s", message);
    return 0;
}

// ========== System Information Functions ==========

int32_t mcp_native_get_free_heap(wasm_exec_env_t exec_env)
{
    return (int32_t)esp_get_free_heap_size();
}

int32_t mcp_native_get_uptime_ms(wasm_exec_env_t exec_env)
{
    return (int32_t)(esp_timer_get_time() / 1000);
}

int32_t mcp_native_get_tick_count(wasm_exec_env_t exec_env)
{
    return (int32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// ========== Pin Access Policy ==========
//
// Every host function that touches a pin goes through host_pin_check(). WASM modules
// can never drive a pin that is:
//   - not a valid GPIO, or wired to the SPI flash (GPIO 6-11)
//   - reserved by the firmware itself: UART0 console, PSRAM, and whatever peripherals
//     are actually in use (SD card bus, I2S) - see mcp_host_reserve_pins()
//   - listed in "protected_pins" in cfg.jsn
// and can never drive an input-only pin as an output (GPIO 34-39, plus "input_only_pins").
// Pins in "max_current_pins" are allowed but switched to the weakest drive strength.

typedef enum
{
    PIN_READ,   // read the level
    PIN_INPUT,  // reconfigure as an input
    PIN_OUTPUT, // drive the pin
} pin_access_t;

#if CONFIG_SPIRAM
// ESP32-WROVER modules wire PSRAM to GPIO16 (CS) and GPIO17 (CLK)
#define BOOT_RESERVED_PSRAM ((1ULL << 16) | (1ULL << 17))
#else
#define BOOT_RESERVED_PSRAM 0
#endif

// GPIO1/GPIO3 are the UART0 console (logs, flashing)
static uint64_t s_reserved_pins = (1ULL << 1) | (1ULL << 3) | BOOT_RESERVED_PSRAM;
static portMUX_TYPE s_pin_lock = portMUX_INITIALIZER_UNLOCKED;

static bool pin_in_list(int pin, const int *list, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (list[i] == pin)
        {
            return true;
        }
    }
    return false;
}

void mcp_host_reserve_pins(const int *pins, int count, const char *owner)
{
    taskENTER_CRITICAL(&s_pin_lock);
    for (int i = 0; i < count; i++)
    {
        if (pins[i] >= 0 && pins[i] < 64)
        {
            s_reserved_pins |= (1ULL << pins[i]);
        }
    }
    taskEXIT_CRITICAL(&s_pin_lock);

    for (int i = 0; i < count; i++)
    {
        ESP_LOGI(TAG, "GPIO %d reserved for %s", pins[i], owner);
    }
}

void mcp_host_release_pins(const int *pins, int count)
{
    taskENTER_CRITICAL(&s_pin_lock);
    for (int i = 0; i < count; i++)
    {
        if (pins[i] >= 0 && pins[i] < 64)
        {
            s_reserved_pins &= ~(1ULL << pins[i]);
        }
    }
    taskEXIT_CRITICAL(&s_pin_lock);
}

// Returns true if the access is allowed. Logs the reason when denied unless op is NULL.
static bool host_pin_check(int32_t pin, pin_access_t access, const char *op)
{
    const char *reason = NULL;
    const gpio_protection_config_t *prot = &g_mcp_config.gpio_protection;

    if (!GPIO_IS_VALID_GPIO(pin))
    {
        reason = "not a valid GPIO";
    }
    else if (pin >= 6 && pin <= 11)
    {
        reason = "wired to SPI flash";
    }
    else if (pin_in_list(pin, prot->protected_pins, prot->protected_pin_count))
    {
        reason = "protected in config";
    }
    else if (access != PIN_READ && (s_reserved_pins & (1ULL << pin)))
    {
        reason = "in use by firmware";
    }
    else if (access == PIN_OUTPUT &&
             (!GPIO_IS_VALID_OUTPUT_GPIO(pin) ||
              pin_in_list(pin, prot->input_only_pins, prot->input_only_pin_count)))
    {
        reason = "input-only pin";
    }

    if (reason && op)
    {
        ESP_LOGW(TAG, "%s: GPIO %ld denied (%s)", op, (long)pin, reason);
    }
    return reason == NULL;
}

// Limit drive strength on pins flagged as high-current risks
static void host_pin_apply_current_limit(int32_t pin)
{
    const gpio_protection_config_t *prot = &g_mcp_config.gpio_protection;
    if (pin_in_list(pin, prot->max_current_pins, prot->max_current_pin_count))
    {
        gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_0);
    }
}

// ========== GPIO Functions (ESP32-specific) ==========

int32_t native_gpio_set_level(wasm_exec_env_t exec_env, int32_t pin, int32_t level)
{
    if (!host_pin_check(pin, PIN_OUTPUT, "gpio_set_level"))
    {
        return -1;
    }

    esp_err_t result = gpio_set_level((gpio_num_t)pin, level);
    return (result == ESP_OK) ? 0 : -1;
}

int32_t native_gpio_get_level(wasm_exec_env_t exec_env, int32_t pin)
{
    if (!host_pin_check(pin, PIN_READ, "gpio_get_level"))
    {
        return -1;
    }

    return gpio_get_level((gpio_num_t)pin);
}

int32_t native_gpio_set_direction(wasm_exec_env_t exec_env, int32_t pin, int32_t direction)
{
    gpio_mode_t mode;
    switch (direction)
    {
    case 0:
        mode = GPIO_MODE_INPUT;
        break;
    case 1:
        mode = GPIO_MODE_OUTPUT;
        break;
    case 2:
        mode = GPIO_MODE_INPUT_OUTPUT;
        break;
    default:
        return -1; // Invalid direction
    }

    if (!host_pin_check(pin, mode == GPIO_MODE_INPUT ? PIN_INPUT : PIN_OUTPUT, "gpio_set_direction"))
    {
        return -1;
    }

    esp_err_t result = gpio_set_direction((gpio_num_t)pin, mode);
    if (result == ESP_OK && mode != GPIO_MODE_INPUT)
    {
        host_pin_apply_current_limit(pin);
    }
    return (result == ESP_OK) ? 0 : -1;
}

// Lets a module ask before trying; enforcement happens in the functions above regardless
int32_t native_gpio_is_pin_protected(wasm_exec_env_t exec_env, int32_t pin)
{
    return host_pin_check(pin, PIN_OUTPUT, NULL) ? 0 : 1;
}

// ========== PWM Functions (LEDC-based for Music/Speaker Control) ==========

#define PWM_TIMER_RESOLUTION LEDC_TIMER_12_BIT
#define PWM_MODE LEDC_LOW_SPEED_MODE
#define PWM_TIMER LEDC_TIMER_0
#define PWM_CHANNEL LEDC_CHANNEL_0
#define PWM_DUTY_MAX ((1 << PWM_TIMER_RESOLUTION) - 1) // 4095 for 12-bit

static bool pwm_initialized = false;
static int32_t pwm_current_pin = -1;

int32_t native_pwm_init(wasm_exec_env_t exec_env, int32_t pin, int32_t frequency)
{
    // Validate parameters
    if (pin < 0 || pin >= 40)
    {
        ESP_LOGE(TAG, "PWM: Invalid pin %d", pin);
        return -1;
    }

    if (frequency < 1 || frequency > 40000)
    { // 1Hz to 40kHz range
        ESP_LOGE(TAG, "PWM: Invalid frequency %d Hz", frequency);
        return -2;
    }

    if (!host_pin_check(pin, PIN_OUTPUT, "pwm_init"))
    {
        return -3;
    }

    esp_err_t err;

    // Configure timer
    ledc_timer_config_t timer_config = {
        .duty_resolution = PWM_TIMER_RESOLUTION,
        .freq_hz = frequency,
        .speed_mode = PWM_MODE,
        .timer_num = PWM_TIMER,
        .clk_cfg = LEDC_AUTO_CLK};

    err = ledc_timer_config(&timer_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "PWM: Timer config failed: %s", esp_err_to_name(err));
        return -4;
    }

    // Configure channel
    ledc_channel_config_t channel_config = {
        .channel = PWM_CHANNEL,
        .duty = 0, // Start with 0% duty
        .gpio_num = pin,
        .speed_mode = PWM_MODE,
        .hpoint = 0,
        .timer_sel = PWM_TIMER};

    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "PWM: Channel config failed: %s", esp_err_to_name(err));
        return -5;
    }

    host_pin_apply_current_limit(pin);
    pwm_initialized = true;
    pwm_current_pin = pin;

    ESP_LOGI(TAG, "PWM: Initialized pin %d at %d Hz", pin, frequency);
    return 0;
}

int32_t native_pwm_set_duty(wasm_exec_env_t exec_env, int32_t pin, int32_t duty_percent)
{
    if (!pwm_initialized || pwm_current_pin != pin)
    {
        ESP_LOGE(TAG, "PWM: Not initialized for pin %d", pin);
        return -1;
    }

    if (duty_percent < 0 || duty_percent > 100)
    {
        ESP_LOGE(TAG, "PWM: Invalid duty cycle %d%% (0-100 required)", duty_percent);
        return -2;
    }

    // Convert percentage to duty value (0-4095 for 12-bit)
    uint32_t duty = (duty_percent * PWM_DUTY_MAX) / 100;

    esp_err_t err = ledc_set_duty(PWM_MODE, PWM_CHANNEL, duty);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "PWM: Set duty failed: %s", esp_err_to_name(err));
        return -3;
    }

    err = ledc_update_duty(PWM_MODE, PWM_CHANNEL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "PWM: Update duty failed: %s", esp_err_to_name(err));
        return -4;
    }

    ESP_LOGD(TAG, "PWM: Pin %d duty set to %d%% (raw: %lu)", pin, duty_percent, duty);
    return 0;
}

int32_t native_pwm_stop(wasm_exec_env_t exec_env, int32_t pin)
{
    if (!pwm_initialized || pwm_current_pin != pin)
    {
        ESP_LOGW(TAG, "PWM: Stop called but not initialized for pin %d", pin);
        return 0; // Not an error, just no-op
    }

    // Set duty to 0 to stop output
    esp_err_t err = ledc_set_duty(PWM_MODE, PWM_CHANNEL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "PWM: Stop duty set failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = ledc_update_duty(PWM_MODE, PWM_CHANNEL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "PWM: Stop duty update failed: %s", esp_err_to_name(err));
        return -2;
    }

    // Optional: Stop the timer (this will affect all channels using this timer)
    // ledc_timer_pause(PWM_MODE, PWM_TIMER);

    ESP_LOGI(TAG, "PWM: Stopped pin %d", pin);
    return 0;
}

int32_t native_pwm_set_frequency(wasm_exec_env_t exec_env, int32_t pin, int32_t frequency)
{
    if (!pwm_initialized || pwm_current_pin != pin)
    {
        ESP_LOGE(TAG, "PWM: Not initialized for pin %d", pin);
        return -1;
    }

    if (frequency < 1 || frequency > 40000)
    {
        ESP_LOGE(TAG, "PWM: Invalid frequency %d Hz", frequency);
        return -2;
    }

    // Update timer frequency
    esp_err_t err = ledc_set_freq(PWM_MODE, PWM_TIMER, frequency);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "PWM: Set frequency failed: %s", esp_err_to_name(err));
        return -3;
    }

    ESP_LOGD(TAG, "PWM: Pin %d frequency updated to %d Hz", pin, frequency);
    return 0;
}

// ========== WAMR Memory Allocator Access Functions ==========

void mcp_get_wamr_allocators(void **malloc_func, void **realloc_func, void **free_func)
{
    if (malloc_func)
        *malloc_func = (void *)wamr_malloc;
    if (realloc_func)
        *realloc_func = (void *)wamr_realloc;
    if (free_func)
        *free_func = (void *)wamr_free;
}

// ========== SSE Event Functions (MCP Protocol) ==========

int32_t native_sse_send_event(wasm_exec_env_t exec_env, const char *event_type, const char *data)
{
    if (!event_type || !data)
    {
        ESP_LOGE(TAG, "SSE_SEND_EVENT: Invalid parameters - event_type=%p, data=%p", event_type, data);
        return -1;
    }

    ESP_LOGD(TAG, "WASM sending SSE event: %s = %s", event_type, data);

    // Send event to current session using session manager
    esp_err_t ret = mcp_session_send_to_current(event_type, data);

    if (ret == ESP_OK)
    {
        ESP_LOGD(TAG, "SSE event sent successfully");
        return 0;
    }
    else if (ret == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "No current session or SSE connection for event");
        return -2;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send SSE event: %s", esp_err_to_name(ret));
        return -1;
    }
}

// Escape a string for use inside a JSON string literal. Always NUL-terminates;
// returns false if the output was truncated.
static bool json_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (; *in; in++)
    {
        unsigned char c = (unsigned char)*in;
        char esc[7];
        size_t n;
        if (c == '"' || c == '\\')
        {
            esc[0] = '\\';
            esc[1] = (char)c;
            n = 2;
        }
        else if (c == '\n' || c == '\r' || c == '\t')
        {
            esc[0] = '\\';
            esc[1] = c == '\n' ? 'n' : (c == '\r' ? 'r' : 't');
            n = 2;
        }
        else if (c < 0x20)
        {
            n = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", c);
        }
        else
        {
            esc[0] = (char)c;
            n = 1;
        }
        if (o + n >= out_size)
        {
            out[o] = '\0';
            return false;
        }
        memcpy(out + o, esc, n);
        o += n;
    }
    out[o] = '\0';
    return true;
}

int32_t native_sse_send_tool_result(wasm_exec_env_t exec_env, const char *tool_name, const char *result)
{
    if (!tool_name || !result)
    {
        ESP_LOGE(TAG, "SSE_SEND_TOOL_RESULT: Invalid parameters - tool_name=%p, result=%p", tool_name, result);
        return -1;
    }

    // Create JSON-formatted tool result. Both values are module-supplied text, so
    // escape them rather than splicing them into the JSON raw.
    char tool_esc[96];
    char result_esc[384];
    json_escape(tool_name, tool_esc, sizeof(tool_esc));
    json_escape(result, result_esc, sizeof(result_esc));

    char event_data[512];
    int len = snprintf(event_data, sizeof(event_data), "{\"tool\":\"%s\",\"result\":\"%s\"}", tool_esc, result_esc);

    if (len >= sizeof(event_data))
    {
        ESP_LOGE(TAG, "Tool result too large for buffer (%d >= %zu)", len, sizeof(event_data));
        return -3;
    }

    ESP_LOGD(TAG, "WASM tool result: %s", event_data);

    // Send tool result event to current session
    esp_err_t ret = mcp_session_send_to_current("tool_result", event_data);

    if (ret == ESP_OK)
    {
        ESP_LOGD(TAG, "Tool result sent successfully");
        return 0;
    }
    else if (ret == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "No current session or SSE connection for tool result");
        return -2;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send tool result: %s", esp_err_to_name(ret));
        return -1;
    }
}

// ========== JSON-RPC Protocol Functions (MCP Framework) ==========

int32_t native_json_parse_request(wasm_exec_env_t exec_env, const char *json_str,
                                  char *method_out, char *params_out, int32_t max_len)
{
    ESP_LOGD(TAG, "JSON_PARSE: Called with json=%p, method=%p, params=%p, max_len=%d", json_str, method_out, params_out, max_len);
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

    // "*" arguments are only checked by WAMR for a single byte. Validate the full
    // extent of both output buffers before writing anything into them.
    if (!method_out || !params_out || max_len < 5 ||
        !wasm_runtime_validate_native_addr(module_inst, method_out, 64) ||
        !wasm_runtime_validate_native_addr(module_inst, params_out, (uint64_t)max_len))
    {
        ESP_LOGE(TAG, "JSON_PARSE: output buffers outside module memory (max_len=%ld)", (long)max_len);
        return -1;
    }

    ESP_LOGD(TAG, "JSON_PARSE: All validations passed, JSON content: '%.50s'", json_str);

    // Parse JSON-RPC method
    ESP_LOGD(TAG, "JSON_PARSE: About to call json_find_string");
    if (!mcp_json_find_string(json_str, "method", method_out, 64))
    { // method buffer is 64 bytes
        ESP_LOGE(TAG, "JSON_PARSE: json_find_string failed - method not found");
        return -2; // Method not found
    }

    // Convert host pointer back to WASM address
    // uint32_t method_out = wasm_runtime_addr_native_to_app(module_inst, method);

    ESP_LOGD(TAG, "JSON_PARSE: Successfully parsed method: '%s'", method_out);

    // Parse JSON-RPC params (extract the params object as string)
    const char *params_start = strstr(json_str, "\"params\"");
    if (params_start)
    {
        params_start = strchr(params_start, ':');
        if (params_start)
        {
            params_start++; // Skip ':'
            while (*params_start == ' ' || *params_start == '\t')
                params_start++; // Skip whitespace

            // Copy the params value
            int i = 0;
            int brace_depth = 0;
            bool in_string = false;
            bool escape_next = false;

            while (params_start[i] && i < max_len - 1)
            {
                char c = params_start[i];

                if (escape_next)
                {
                    escape_next = false;
                }
                else if (c == '\\')
                {
                    escape_next = true;
                }
                else if (c == '"' && !escape_next)
                {
                    in_string = !in_string;
                }
                else if (!in_string)
                {
                    if (c == '{')
                        brace_depth++;
                    else if (c == '}')
                        brace_depth--;
                    else if ((c == ',' || c == '}') && brace_depth == 0)
                        break;
                }

                params_out[i] = c;
                i++;

                if (!in_string && brace_depth == 0 && (c == ']' || c == '"' || (c >= '0' && c <= '9')))
                {
                    // End of simple value
                    break;
                }
            }
            params_out[i] = '\0'; // i <= max_len - 1
        }
        else
        {
            snprintf(params_out, max_len, "null");
        }
    }
    else
    {
        snprintf(params_out, max_len, "null");
    }

    return 0;
}

int32_t native_json_create_response(wasm_exec_env_t exec_env, const char *result, char *response, int32_t max_len)
{
    // Using buffer + length signature ($*~) - safer parameter handling
    if (!response || max_len <= 0)
    {
        printf("ERROR: Invalid response buffer parameters\\n");
        return -1;
    }

    int len = snprintf(response, max_len,
                       "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":1}",
                       result);

    printf("JSON_CREATE_RESPONSE: Created response (%d bytes): %.100s...\\n", len, response);
    return len < max_len ? 0 : -2; // Buffer overflow
}

int32_t native_json_create_error(wasm_exec_env_t exec_env, int32_t code, const char *message, char *response, int32_t max_len)
{
    // Using buffer + length signature (i$*~) - safer parameter handling
    if (!response || max_len <= 0)
    {
        printf("ERROR: Invalid response buffer parameters\\n");
        return -1;
    }

    int len = snprintf(response, max_len,
                       "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%ld,\"message\":\"%s\"},\"id\":1}",
                       (long)code, message);

    printf("JSON_CREATE_ERROR: Created error response (%d bytes): %.100s...\\n", len, response);
    return len < max_len ? 0 : -2; // Buffer overflow
}

// ========== Request Context Management ==========

void mcp_host_set_request_context(void *request)
{
    tls_current_request = (mcp_request_t *)request;
}

void mcp_host_clear_request_context(void)
{
    tls_current_request = NULL;
}

// ========== HTTP Header Management Functions ==========

// Helper function to ensure response headers buffer has enough capacity
static int ensure_response_headers_capacity(mcp_request_t *request, size_t needed)
{
    if (!request->response_headers)
    {
        // Initial allocation
        request->response_headers_capacity = needed > 1024 ? needed : 1024;
        request->response_headers = heap_caps_malloc(request->response_headers_capacity, MALLOC_CAP_SPIRAM);
        if (!request->response_headers)
            return -1;
        request->response_headers[0] = '\0';
        request->response_headers_length = 0;
    }
    else if (request->response_headers_length + needed >= request->response_headers_capacity)
    {
        // Realloc to larger size
        size_t new_capacity = (request->response_headers_capacity + needed) * 2;
        char *new_buffer = heap_caps_realloc(request->response_headers, new_capacity, MALLOC_CAP_SPIRAM);
        if (!new_buffer)
            return -1;
        request->response_headers = new_buffer;
        request->response_headers_capacity = new_capacity;
    }
    return 0;
}

int32_t mcp_native_set_response_header(wasm_exec_env_t exec_env, const char *name, const char *value)
{
    if (!tls_current_request)
    {
        ESP_LOGE(TAG, "set_response_header: No current request context");
        return -1;
    }

    if (!name || !value)
    {
        ESP_LOGE(TAG, "set_response_header: Invalid parameters");
        return -1;
    }

    // Validate header name (basic ASCII check)
    for (const char *p = name; *p; p++)
    {
        if (*p < 0x21 || *p > 0x7E || *p == ':')
        {
            ESP_LOGE(TAG, "set_response_header: Invalid header name");
            return -1;
        }
    }

    // Calculate needed space: "Name: Value\r\n"
    size_t needed = strlen(name) + strlen(value) + 4; // ": " + "\r\n"

    if (ensure_response_headers_capacity(tls_current_request, needed) != 0)
    {
        ESP_LOGE(TAG, "set_response_header: Memory allocation failed");
        return -2;
    }

    // Check if header already exists and remove it first
    mcp_native_remove_response_header(exec_env, name);

    // Append new header
    int len = snprintf(tls_current_request->response_headers + tls_current_request->response_headers_length,
                       tls_current_request->response_headers_capacity - tls_current_request->response_headers_length,
                       "%s: %s\r\n", name, value);

    if (len > 0)
    {
        tls_current_request->response_headers_length += len;
        ESP_LOGD(TAG, "set_response_header: %s = %s", name, value);
        return 0;
    }

    return -3;
}

// Get request header value (case-insensitive) - returns length or -1 if not found
int32_t mcp_native_get_request_header(wasm_exec_env_t exec_env, const char *name, char *buffer, int32_t buffer_size)
{
    if (!tls_current_request)
    {
        ESP_LOGW(TAG, "get_request_header: No current request context");
        return -1;
    }

    if (!name || !buffer || buffer_size <= 0)
    {
        ESP_LOGE(TAG, "get_request_header: Invalid parameters");
        return -1;
    }

    if (!tls_current_request->headers)
    {
        return -1; // No headers
    }

    // Inline header search - no static buffers!
    size_t name_len = strlen(name);
    const char *pos = tls_current_request->headers;

    while (*pos)
    {
        // Find header name (before colon)
        const char *colon = strchr(pos, ':');
        if (!colon)
            break;

        // Check if header name matches (case-insensitive)
        if ((colon - pos) == name_len && strncasecmp(pos, name, name_len) == 0)
        {
            // Found header, skip colon and whitespace
            const char *value = colon + 1;
            while (*value == ' ' || *value == '\t')
                value++;

            // Find end of value (before \r\n)
            const char *end = strstr(value, "\r\n");
            if (!end)
                end = value + strlen(value);

            // Copy directly to provided buffer - no static allocation!
            size_t value_len = end - value;
            if (value_len >= buffer_size)
                value_len = buffer_size - 1;

            strncpy(buffer, value, value_len);
            buffer[value_len] = '\0';

            ESP_LOGD(TAG, "get_request_header: %s = %s", name, buffer);
            return strlen(buffer);
        }

        // Move to next line
        const char *next_line = strstr(pos, "\r\n");
        if (!next_line)
            break;
        pos = next_line + 2;
    }

    ESP_LOGD(TAG, "get_request_header: %s not found", name);
    return -1; // Not found
}

int32_t mcp_native_remove_response_header(wasm_exec_env_t exec_env, const char *name)
{
    if (!tls_current_request || !tls_current_request->response_headers)
    {
        return -1;
    }

    if (!name)
    {
        return -1;
    }

    char *headers = tls_current_request->response_headers;
    size_t name_len = strlen(name);
    char *pos = headers;

    while (*pos)
    {
        // Find header name
        char *colon = strchr(pos, ':');
        if (!colon)
            break;

        // Check if this is the header to remove
        if ((colon - pos) == name_len && strncasecmp(pos, name, name_len) == 0)
        {
            // Found header to remove, find end of line
            char *line_end = strstr(pos, "\r\n");
            if (line_end)
            {
                line_end += 2; // Include \r\n
                // Move remaining content forward
                size_t remaining = strlen(line_end);
                memmove(pos, line_end, remaining + 1);
                tls_current_request->response_headers_length -= (line_end - pos);
                ESP_LOGD(TAG, "remove_response_header: Removed %s", name);
                return 0;
            }
        }

        // Move to next line
        char *next_line = strstr(pos, "\r\n");
        if (!next_line)
            break;
        pos = next_line + 2;
    }

    return -1; // Header not found
}

// List all request headers as comma-separated names - returns length or -1 if error
int32_t mcp_native_list_request_headers(wasm_exec_env_t exec_env, char *buffer, int32_t buffer_size)
{
    if (!buffer || buffer_size <= 0)
    {
        return -1;
    }

    if (!tls_current_request || !tls_current_request->headers)
    {
        buffer[0] = '\0';
        return 0; // Empty list
    }

    buffer[0] = '\0';
    const char *pos = tls_current_request->headers;
    size_t list_len = 0;

    while (*pos && list_len < buffer_size - 50)
    {
        // Find header name (before colon)
        const char *colon = strchr(pos, ':');
        if (!colon)
            break;

        // Add comma if not first header
        if (list_len > 0 && list_len < buffer_size - 1)
        {
            strcat(buffer, ",");
            list_len++;
        }

        // Add header name
        size_t name_len = colon - pos;
        if (list_len + name_len < buffer_size - 1)
        {
            strncat(buffer, pos, name_len);
            list_len += name_len;
        }

        // Move to next line
        const char *next_line = strstr(pos, "\r\n");
        if (!next_line)
            break;
        pos = next_line + 2;
    }

    ESP_LOGD(TAG, "list_request_headers: %s", buffer);
    return strlen(buffer);
}

// ========== I2S Audio Functions ==========

// I2S configuration and state
static bool i2s_initialized = false;
static int32_t i2s_sample_rate = 44100;
static int32_t i2s_bits_per_sample = 16;
static int32_t i2s_volume = 50; // Default 50%
static bool i2s_playing = false;
static esp_timer_handle_t i2s_timer_handle = NULL;
// Tone and melody playback each run in their own task. A task clears its handle
// as the last thing it does, so "handle == NULL" means it has fully stopped.
static TaskHandle_t volatile i2s_tone_task_handle = NULL;
static TaskHandle_t volatile i2s_melody_task_handle = NULL;

// Serializes the audio API (play/stop/deinit) across worker threads, so two
// modules can't start or tear down playback underneath each other
static pthread_mutex_t s_audio_mutex = PTHREAD_MUTEX_INITIALIZER;

// Longest a single i2s_write may block. Normal writes finish in well under the
// DMA queue length (~370ms); this keeps a task from hanging if I2S is stopped.
#define I2S_WRITE_TIMEOUT_MS 1000

static bool wait_for_task_exit(TaskHandle_t volatile *handle, int timeout_ms)
{
    for (int waited = 0; *handle && waited < timeout_ms; waited += 10)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return *handle == NULL;
}

// I2S pin configuration (FreenoVE Audio Converter & Amplifier)
#define I2S_BCK_PIN 27  // Bit Clock
#define I2S_WS_PIN 26   // Word Select (LRC)
#define I2S_DATA_PIN 25 // Data Input (DIN)
static const int i2s_pins[] = {I2S_BCK_PIN, I2S_WS_PIN, I2S_DATA_PIN};

// Audio generation parameters
#define I2S_SAMPLE_BUFFER_SIZE 64  // Increased from 16 for smoother playback
#define I2S_DMA_DESC_NUM 32   // was 8
#define I2S_DMA_FRAME_NUM 512 // was 64
#define SINE_TABLE_SIZE 1024

// Pre-computed sine wave table for efficiency
static int16_t *sine_table = NULL;
static float current_frequency = 0.0f;
static double phase_accumulator = 0.0f;

// ========== Envelope Support for Natural Sound ==========

// Envelope Types
typedef enum {
    ENVELOPE_LINEAR = 0,      // Simple linear ramp
    ENVELOPE_EXPONENTIAL = 1, // Natural exponential curve
    ENVELOPE_COSINE = 2       // Smooth cosine curve
} envelope_type_t;

// Envelope Phases
typedef enum {
    ENVELOPE_IDLE = 0,
    ENVELOPE_ATTACK = 1,
    ENVELOPE_SUSTAIN = 2,
    ENVELOPE_RELEASE = 3
} envelope_phase_t;

// Envelope State
typedef struct {
    envelope_type_t type;          // Type of envelope curve
    uint32_t attack_samples;       // Attack duration in samples
    uint32_t release_samples;      // Release duration in samples
    float sustain_level;           // Sustain amplitude (0.0-1.0)
    uint32_t current_sample;       // Current position in envelope
    envelope_phase_t phase;        // Current envelope phase
    uint32_t phase_start_sample;   // Sample count when phase started
    float last_amplitude;          // Last calculated amplitude for smooth transitions
    uint32_t total_samples;        // Total samples for the note (for auto-release)
} envelope_state_t;

// Global envelope state
static envelope_state_t current_envelope = {
    .type = ENVELOPE_EXPONENTIAL,
    .attack_samples = 0,    // Will be initialized based on ms
    .release_samples = 0,   // Will be initialized based on ms
    .sustain_level = 0.9f,
    .current_sample = 0,
    .phase = ENVELOPE_IDLE,
    .phase_start_sample = 0,
    .last_amplitude = 0.0f,
    .total_samples = 0
};

// Default envelope parameters (in milliseconds)
#define DEFAULT_ATTACK_MS  20
#define DEFAULT_RELEASE_MS 100
#define DEFAULT_SUSTAIN_LEVEL 0.9f

// Current envelope settings (can be changed via i2s_set_envelope)
static uint32_t envelope_attack_ms = DEFAULT_ATTACK_MS;
static uint32_t envelope_release_ms = DEFAULT_RELEASE_MS;

// ========== Melody Support for Continuous Playback ==========

// Structure for a single note in a melody
typedef struct {
    float frequency;           // Note frequency in Hz
    uint32_t duration_ms;      // Note duration in milliseconds
    uint32_t start_sample;     // Sample index when note starts
    uint32_t end_sample;       // Sample index when note ends
} melody_note_t;

// Dual oscillator state for crossfading
typedef struct {
    float frequency;           // Current frequency
    double phase_accumulator;  // Phase for sine generation
    envelope_state_t envelope; // Envelope state for this oscillator
    float amplitude;          // Current amplitude (0.0-1.0)
    bool active;              // Is this oscillator active?
} oscillator_state_t;

// Melody playback state
typedef struct {
    melody_note_t *notes;      // Array of parsed notes
    int note_count;            // Total number of notes
    int current_note_idx;      // Index of currently playing note
    uint32_t total_samples;    // Total samples for entire melody
    uint32_t current_sample;   // Current playback position
    bool is_playing;           // Is melody currently playing?
    
    // Dual oscillators for crossfading
    oscillator_state_t osc_a;  // Primary oscillator
    oscillator_state_t osc_b;  // Secondary oscillator
    
    // Crossfade settings
    uint32_t crossfade_samples; // Duration of crossfade in samples
    bool crossfading;          // Are we currently crossfading?
    uint32_t crossfade_start;  // Sample index when crossfade started
} melody_state_t;

// Global melody state
static melody_state_t melody_state = {
    .notes = NULL,
    .note_count = 0,
    .current_note_idx = 0,
    .total_samples = 0,
    .current_sample = 0,
    .is_playing = false,
    .osc_a = {.active = false},
    .osc_b = {.active = false},
    .crossfade_samples = 0,
    .crossfading = false,
    .crossfade_start = 0
};

// Default crossfade duration in milliseconds
#define DEFAULT_CROSSFADE_MS 30

// Calculate envelope amplitude for current sample
static float calculate_envelope_amplitude(envelope_state_t *env) {
    if (!env || env->phase == ENVELOPE_IDLE) {
        return 0.0f;
    }
    
    float amplitude = 0.0f;
    uint32_t samples_in_phase = env->current_sample - env->phase_start_sample;
    
    switch (env->phase) {
        case ENVELOPE_ATTACK:
            if (env->attack_samples == 0) {
                // Instant attack
                amplitude = env->sustain_level;
            } else {
                switch (env->type) {
                    case ENVELOPE_LINEAR:
                        // Linear ramp from 0 to sustain_level
                        amplitude = (float)samples_in_phase / (float)env->attack_samples * env->sustain_level;
                        break;
                    
                    case ENVELOPE_EXPONENTIAL:
                        // Exponential curve: 1 - e^(-5*t) gives ~99% at t=1
                        float t = (float)samples_in_phase / (float)env->attack_samples;
                        amplitude = env->sustain_level * (1.0f - expf(-5.0f * t));
                        break;
                    
                    case ENVELOPE_COSINE:
                        // Cosine curve for smooth attack
                        float phase = (float)samples_in_phase / (float)env->attack_samples * M_PI;
                        amplitude = env->sustain_level * (1.0f - cosf(phase)) * 0.5f;
                        break;
                }
            }
            
            // Check if attack phase is complete
            if (samples_in_phase >= env->attack_samples) {
                env->phase = ENVELOPE_SUSTAIN;
                env->phase_start_sample = env->current_sample;
                amplitude = env->sustain_level;
            }
            break;
        
        case ENVELOPE_SUSTAIN:
            amplitude = env->sustain_level;
            
            // Check if it's time to start release (for notes with duration)
            if (env->total_samples > 0 && 
                env->current_sample >= (env->total_samples - env->release_samples)) {
                env->phase = ENVELOPE_RELEASE;
                env->phase_start_sample = env->current_sample;
            }
            break;
        
        case ENVELOPE_RELEASE:
            if (env->release_samples == 0) {
                // Instant release
                amplitude = 0.0f;
            } else {
                switch (env->type) {
                    case ENVELOPE_LINEAR:
                        // Linear ramp from sustain_level to 0
                        amplitude = env->sustain_level * (1.0f - (float)samples_in_phase / (float)env->release_samples);
                        break;
                    
                    case ENVELOPE_EXPONENTIAL:
                        // Exponential decay
                        float t = (float)samples_in_phase / (float)env->release_samples;
                        amplitude = env->sustain_level * expf(-5.0f * t);
                        break;
                    
                    case ENVELOPE_COSINE:
                        // Cosine curve for smooth release
                        float phase = (float)samples_in_phase / (float)env->release_samples * M_PI;
                        amplitude = env->sustain_level * cosf(phase) * 0.5f + env->sustain_level * 0.5f;
                        break;
                }
            }
            
            // Clamp to 0 if release is complete
            if (samples_in_phase >= env->release_samples) {
                amplitude = 0.0f;
                env->phase = ENVELOPE_IDLE;
            }
            break;
        
        default:
            amplitude = 0.0f;
            break;
    }
    
    // Clamp amplitude to valid range
    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;
    
    env->last_amplitude = amplitude;
    return amplitude;
}

// Initialize envelope for a new note
static void init_envelope_for_note(envelope_state_t *env, uint32_t duration_ms, uint32_t sample_rate) {
    // Convert milliseconds to samples using current envelope settings
    // Make sure we have sensible defaults if not set
    if (envelope_attack_ms == 0) envelope_attack_ms = DEFAULT_ATTACK_MS;
    if (envelope_release_ms == 0) envelope_release_ms = DEFAULT_RELEASE_MS;
    
    env->attack_samples = (envelope_attack_ms * sample_rate) / 1000;
    env->release_samples = (envelope_release_ms * sample_rate) / 1000;
    env->sustain_level = DEFAULT_SUSTAIN_LEVEL;  // Make sure sustain level is set!
    
    // Calculate total samples for the note
    if (duration_ms > 0) {
        env->total_samples = (duration_ms * sample_rate) / 1000;
    } else {
        env->total_samples = 0; // Continuous tone
    }
    
    // Reset envelope state
    env->current_sample = 0;
    env->phase = ENVELOPE_ATTACK;
    env->phase_start_sample = 0;
    env->last_amplitude = 0.0f;
    env->type = current_envelope.type;  // Use the global envelope type setting
    
    // Debug: Log envelope initialization
    ESP_LOGI(TAG, "Envelope init: attack=%lu samples, release=%lu samples, total=%lu, type=%d, sustain=%.2f", 
             env->attack_samples, env->release_samples, env->total_samples, 
             env->type, env->sustain_level);
}

// Initialize sine wave lookup table
static esp_err_t init_sine_table(void)
{
    if (sine_table != NULL)
        return ESP_OK;

    // Use smart allocation - sine table can go in PSRAM for memory efficiency
    sine_table = (int16_t *)mcp_malloc(SINE_TABLE_SIZE * sizeof(int16_t));
    if (!sine_table)
    {
        ESP_LOGE(TAG, "Failed to allocate sine table");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < SINE_TABLE_SIZE; i++)
    {
        float angle = 2.0f * M_PI * i / SINE_TABLE_SIZE;
        sine_table[i] = (int16_t)(sin(angle) * 32767);
    }

    ESP_LOGI(TAG, "Sine table initialized (%d samples)", SINE_TABLE_SIZE);
    return ESP_OK;
}

// Audio generation task
static void i2s_audio_task(void *pvParameters)
{
    // Double buffering implementation for smooth audio playback
    size_t buffer_size = I2S_SAMPLE_BUFFER_SIZE * sizeof(int16_t) * 2;

    // Allocate TWO buffers for double buffering
    int16_t *buffer_A = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    int16_t *buffer_B = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    if (!buffer_A || !buffer_B)
    {
        ESP_LOGE(TAG, "Failed to allocate I2S double buffers");
        if (buffer_A)
            heap_caps_free(buffer_A);
        if (buffer_B)
            heap_caps_free(buffer_B);
        i2s_playing = false;
        i2s_tone_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Buffer pointers for swapping
    int16_t *current_buffer = buffer_A;
    int16_t *next_buffer = buffer_B;

    size_t bytes_written;
    double phase_increment = (current_frequency * SINE_TABLE_SIZE) / i2s_sample_rate;

    // Pre-fill the first buffer before entering the loop
    for (int i = 0; i < I2S_SAMPLE_BUFFER_SIZE; i++)
    {
        int table_index = (int)fmod(phase_accumulator, SINE_TABLE_SIZE);
        
        // Get base sine value
        int16_t base_sample = sine_table[table_index];
        
        // Apply envelope amplitude
        float envelope_amp = calculate_envelope_amplitude(&current_envelope);
        
        // Apply both envelope and volume
        int16_t sample = (int16_t)((base_sample * envelope_amp * i2s_volume) / 100.0f);

        current_buffer[i * 2] = sample;     // Left channel
        current_buffer[i * 2 + 1] = sample; // Right channel

        phase_accumulator = fmod(phase_accumulator + phase_increment, SINE_TABLE_SIZE);
        
        // Increment envelope sample counter
        current_envelope.current_sample++;
    }

    while (i2s_playing)
    {
        // Start writing current buffer to I2S (this will block until DMA accepts it)
        esp_err_t ret = i2s_write(I2S_NUM_0, current_buffer,
                                  buffer_size,
                                  &bytes_written, pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));

        // esp_err_t ret = i2s_write(I2S_NUM_0, current_buffer, buffer_size,
        //                           &bytes_written, pdMS_TO_TICKS(10));

        if (bytes_written != buffer_size)
        {
            ESP_LOGW(TAG, "Underrun: wrote %d of %d", bytes_written, buffer_size);
        }

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            break;
        }

        // While current buffer is being sent via DMA, prepare the next buffer
        for (int i = 0; i < I2S_SAMPLE_BUFFER_SIZE; i++)
        {
            int table_index = (int)fmod(phase_accumulator, SINE_TABLE_SIZE);
            
            // Get base sine value
            int16_t base_sample = sine_table[table_index];
            
            // Apply envelope amplitude
            float envelope_amp = calculate_envelope_amplitude(&current_envelope);
            
            // Apply both envelope and volume
            int16_t sample = (int16_t)((base_sample * envelope_amp * i2s_volume) / 100.0f);

            next_buffer[i * 2] = sample;     // Left channel
            next_buffer[i * 2 + 1] = sample; // Right channel

            phase_accumulator = fmod(phase_accumulator + phase_increment, SINE_TABLE_SIZE);
            
            // Increment envelope sample counter
            current_envelope.current_sample++;
        }

        // Swap buffers - next becomes current, current becomes next
        int16_t *temp = current_buffer;
        current_buffer = next_buffer;
        next_buffer = temp;

        // No delay needed - i2s_write will naturally pace the loop
    }

    // Clean up both buffers
    heap_caps_free(buffer_A);
    heap_caps_free(buffer_B);
    i2s_playing = false;
    i2s_tone_task_handle = NULL;
    vTaskDelete(NULL);
}

// ========== Melody Helper Functions ==========

// Parse a single note string (e.g., "440:500" or just "440")
static int parse_melody_note(const char *note_str, float *frequency, uint32_t *duration_ms) {
    if (!note_str || !frequency || !duration_ms) return -1;
    
    // Default duration if not specified
    *duration_ms = 500;
    
    // Try to parse frequency:duration format
    char *colon = strchr(note_str, ':');
    if (colon) {
        // Parse frequency
        *frequency = atof(note_str);
        // Parse duration
        *duration_ms = atoi(colon + 1);
    } else {
        // Just frequency, use default duration
        *frequency = atof(note_str);
    }
    
    // Validate
    if (*frequency < 20.0f || *frequency > 20000.0f) {
        ESP_LOGE(TAG, "Invalid frequency: %.2f Hz", *frequency);
        return -1;
    }
    
    if (*duration_ms < 10 || *duration_ms > 10000) {
        ESP_LOGE(TAG, "Invalid duration: %lu ms", *duration_ms);
        return -1;
    }
    
    return 0;
}

// Parse melody string into array of notes
static int parse_melody_string(const char *melody_str, melody_note_t **notes_out, int *note_count_out) {
    if (!melody_str || !notes_out || !note_count_out) return -1;
    
    // Count commas to estimate number of notes
    int comma_count = 0;
    for (const char *p = melody_str; *p; p++) {
        if (*p == ',') comma_count++;
    }
    int estimated_notes = comma_count + 1;
    
    // Allocate memory for notes
    melody_note_t *notes = heap_caps_malloc(estimated_notes * sizeof(melody_note_t), MALLOC_CAP_8BIT);
    if (!notes) {
        ESP_LOGE(TAG, "Failed to allocate memory for melody notes");
        return -1;
    }
    
    // Make a copy of the string for parsing
    char *melody_copy = heap_caps_malloc(strlen(melody_str) + 1, MALLOC_CAP_8BIT);
    if (!melody_copy) {
        heap_caps_free(notes);
        return -1;
    }
    strcpy(melody_copy, melody_str);
    
    // Parse each note
    int note_count = 0;
    uint32_t cumulative_samples = 0;
    char *token = strtok(melody_copy, ",");
    
    while (token && note_count < estimated_notes) {
        // Skip whitespace
        while (*token == ' ' || *token == '\t') token++;
        
        float frequency;
        uint32_t duration_ms;
        
        if (parse_melody_note(token, &frequency, &duration_ms) == 0) {
            notes[note_count].frequency = frequency;
            notes[note_count].duration_ms = duration_ms;
            notes[note_count].start_sample = cumulative_samples;
            
            // Calculate samples for this note
            uint32_t note_samples = (duration_ms * i2s_sample_rate) / 1000;
            cumulative_samples += note_samples;
            notes[note_count].end_sample = cumulative_samples;
            
            note_count++;
        }
        
        token = strtok(NULL, ",");
    }
    
    heap_caps_free(melody_copy);
    
    if (note_count == 0) {
        heap_caps_free(notes);
        return -1;
    }
    
    *notes_out = notes;
    *note_count_out = note_count;
    
    return 0;
}

// Initialize oscillator for a note
static void init_oscillator_for_note(oscillator_state_t *osc, float frequency, uint32_t sample_rate) {
    osc->frequency = frequency;
    osc->phase_accumulator = 0.0;
    osc->active = true;
    osc->amplitude = 0.0f;
    
    // Initialize envelope for this oscillator
    init_envelope_for_note(&osc->envelope, 0, sample_rate); // Duration 0 = controlled manually
}

// Generate sample from oscillator with envelope
static int16_t generate_oscillator_sample(oscillator_state_t *osc, uint32_t sample_rate, int volume_percent) {
    if (!osc->active) return 0;
    
    // Calculate phase increment
    double phase_increment = (osc->frequency * SINE_TABLE_SIZE) / sample_rate;
    
    // Get sine value
    int table_index = (int)fmod(osc->phase_accumulator, SINE_TABLE_SIZE);
    int16_t base_sample = sine_table[table_index];
    
    // Apply envelope
    float envelope_amp = calculate_envelope_amplitude(&osc->envelope);
    
    // Debug first few samples
    static int debug_count = 0;
    if (debug_count < 5) {
        ESP_LOGI(TAG, "Osc sample: base=%d, env_amp=%.3f, vol=%d, env.phase=%d, env.current=%lu", 
                 base_sample, envelope_amp, volume_percent, 
                 osc->envelope.phase, osc->envelope.current_sample);
        debug_count++;
    }
    
    // Apply volume and envelope
    int16_t sample = (int16_t)((base_sample * envelope_amp * volume_percent) / 100.0f);
    
    // Update phase
    osc->phase_accumulator = fmod(osc->phase_accumulator + phase_increment, SINE_TABLE_SIZE);
    
    // Update envelope sample counter
    osc->envelope.current_sample++;
    
    return sample;
}

// Continuous melody audio generation task
static void i2s_melody_task(void *pvParameters) {
    // Double buffering for smooth playback
    size_t buffer_size = I2S_SAMPLE_BUFFER_SIZE * sizeof(int16_t) * 2;
    
    // Allocate TWO buffers for double buffering
    int16_t *buffer_A = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    int16_t *buffer_B = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    
    if (!buffer_A || !buffer_B) {
        ESP_LOGE(TAG, "Failed to allocate I2S melody buffers");
        if (buffer_A) heap_caps_free(buffer_A);
        if (buffer_B) heap_caps_free(buffer_B);
        heap_caps_free(melody_state.notes);
        melody_state.notes = NULL;
        melody_state.is_playing = false;
        i2s_melody_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    // Buffer pointers for swapping
    int16_t *current_buffer = buffer_A;
    int16_t *next_buffer = buffer_B;
    size_t bytes_written;
    
    // Start I2S for melody playback
    esp_err_t i2s_ret = i2s_start(I2S_NUM_0);
    if (i2s_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start I2S in melody task: %s", esp_err_to_name(i2s_ret));
        heap_caps_free(buffer_A);
        heap_caps_free(buffer_B);
        heap_caps_free(melody_state.notes);
        melody_state.notes = NULL;
        melody_state.is_playing = false;
        i2s_melody_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "I2S started for melody playback");
    
    // Calculate crossfade samples
    melody_state.crossfade_samples = (DEFAULT_CROSSFADE_MS * i2s_sample_rate) / 1000;
    
    // Initialize first note
    if (melody_state.note_count > 0) {
        ESP_LOGI(TAG, "Initializing first note: freq=%.2f Hz, duration=%lu ms", 
                 melody_state.notes[0].frequency, melody_state.notes[0].duration_ms);
        init_oscillator_for_note(&melody_state.osc_a, 
                                melody_state.notes[0].frequency, 
                                i2s_sample_rate);
        melody_state.current_note_idx = 0;
    }
    
    ESP_LOGI(TAG, "Starting melody playback: %d notes, total_samples=%lu", 
             melody_state.note_count, melody_state.total_samples);
    
    // Check critical system state before starting
    ESP_LOGI(TAG, "Pre-loop checks: sine_table=%p, i2s_sample_rate=%lu, crossfade_samples=%lu", 
             sine_table, i2s_sample_rate, melody_state.crossfade_samples);
    ESP_LOGI(TAG, "Oscillator A: active=%d, freq=%.2f, envelope.phase=%d", 
             melody_state.osc_a.active, melody_state.osc_a.frequency, melody_state.osc_a.envelope.phase);
    ESP_LOGI(TAG, "i2s_volume=%d, buffer_size=%d samples", i2s_volume, I2S_SAMPLE_BUFFER_SIZE);
    
    ESP_LOGI(TAG, "Entering melody playback loop");
    
    while (melody_state.is_playing) {
        ESP_LOGD(TAG, "Processing buffer, current_sample=%lu, current_note=%d", 
                 melody_state.current_sample, melody_state.current_note_idx);
        
        // Fill buffer with samples
        // ESP_LOGD(TAG, "About to fill buffer, I2S_SAMPLE_BUFFER_SIZE=%d", I2S_SAMPLE_BUFFER_SIZE);
        
        for (int i = 0; i < I2S_SAMPLE_BUFFER_SIZE; i++) {
            // Log only occasionally to avoid flooding
            // if (i == 0) {
            //     ESP_LOGD(TAG, "Entering buffer fill loop, sample 0");
            // }
            int32_t mixed_sample = 0;  // Use int32_t for mixing to prevent overflow
            
            // Check if we need to start next note (with crossfade)
            if (melody_state.current_note_idx < melody_state.note_count) {
                melody_note_t *current_note = &melody_state.notes[melody_state.current_note_idx];
                
                // Log only on note changes
                static int last_logged_note = -1;
                if (i == 0 && melody_state.current_note_idx != last_logged_note) {
                    ESP_LOGI(TAG, "Processing note %d: freq=%.2f, start=%lu, end=%lu, current_sample=%lu", 
                             melody_state.current_note_idx, current_note->frequency, 
                             current_note->start_sample, current_note->end_sample, melody_state.current_sample);
                    last_logged_note = melody_state.current_note_idx;
                }
                
                // Check if we're approaching the end of current note and need to start crossfade
                if (!melody_state.crossfading && 
                    melody_state.current_sample >= (current_note->end_sample - melody_state.crossfade_samples) &&
                    melody_state.current_note_idx < melody_state.note_count - 1) {
                    
                    // Start crossfade to next note
                    melody_state.crossfading = true;
                    melody_state.crossfade_start = melody_state.current_sample;
                    
                    // Initialize oscillator B with next note
                    melody_note_t *next_note = &melody_state.notes[melody_state.current_note_idx + 1];
                    init_oscillator_for_note(&melody_state.osc_b, next_note->frequency, i2s_sample_rate);
                    
                    ESP_LOGD(TAG, "Starting crossfade from note %d (%.1fHz) to note %d (%.1fHz)", 
                            melody_state.current_note_idx, current_note->frequency,
                            melody_state.current_note_idx + 1, next_note->frequency);
                }
                
                // Generate samples from active oscillators
                if (melody_state.osc_a.active) {
                    ESP_LOGV(TAG, "Generating sample from osc_a: freq=%.2f", melody_state.osc_a.frequency);
                    int16_t sample_a = generate_oscillator_sample(&melody_state.osc_a, i2s_sample_rate, i2s_volume);
                    
                    if (melody_state.crossfading) {
                        // Calculate crossfade progress (0.0 to 1.0)
                        uint32_t crossfade_progress = melody_state.current_sample - melody_state.crossfade_start;
                        float fade_out = 1.0f - ((float)crossfade_progress / (float)melody_state.crossfade_samples);
                        fade_out = fade_out < 0.0f ? 0.0f : fade_out;
                        sample_a = (int16_t)(sample_a * fade_out);
                    }
                    
                    mixed_sample += sample_a;
                }
                
                if (melody_state.osc_b.active) {
                    int16_t sample_b = generate_oscillator_sample(&melody_state.osc_b, i2s_sample_rate, i2s_volume);
                    
                    if (melody_state.crossfading) {
                        // Calculate crossfade progress (0.0 to 1.0)
                        uint32_t crossfade_progress = melody_state.current_sample - melody_state.crossfade_start;
                        float fade_in = (float)crossfade_progress / (float)melody_state.crossfade_samples;
                        fade_in = fade_in > 1.0f ? 1.0f : fade_in;
                        sample_b = (int16_t)(sample_b * fade_in);
                    }
                    
                    mixed_sample += sample_b;
                }
                
                // Check if no oscillators are active
                if (!melody_state.osc_a.active && !melody_state.osc_b.active) {
                    if (i == 0) { // Log once per buffer
                        ESP_LOGW(TAG, "No oscillators active for note %d! freq=%.2f", 
                                 melody_state.current_note_idx, current_note->frequency);
                    }
                    // Try to activate oscillator A if inactive
                    if (melody_state.current_sample >= current_note->start_sample && 
                        melody_state.current_sample < current_note->end_sample) {
                        ESP_LOGW(TAG, "Activating oscillator A for note %d", melody_state.current_note_idx);
                        init_oscillator_for_note(&melody_state.osc_a, current_note->frequency, i2s_sample_rate);
                    }
                }
                
                // Check if crossfade is complete
                if (melody_state.crossfading && 
                    (melody_state.current_sample - melody_state.crossfade_start) >= melody_state.crossfade_samples) {
                    
                    // Swap oscillators: B becomes A, A becomes inactive
                    melody_state.osc_a = melody_state.osc_b;
                    melody_state.osc_b.active = false;
                    melody_state.crossfading = false;
                    melody_state.current_note_idx++;
                    
                    ESP_LOGD(TAG, "Crossfade complete, now playing note %d", melody_state.current_note_idx);
                }
                
                // Check if current note is complete (no crossfade) - only for last note or error cases
                if (!melody_state.crossfading && 
                    melody_state.current_sample >= current_note->end_sample &&
                    melody_state.current_note_idx == melody_state.note_count - 1) {
                    // Last note completed
                    melody_state.current_note_idx++;
                }
            }
            
            // Clamp mixed sample to int16_t range
            if (mixed_sample > INT16_MAX) mixed_sample = INT16_MAX;
            if (mixed_sample < INT16_MIN) mixed_sample = INT16_MIN;
            
            int16_t final_sample = (int16_t)mixed_sample;
            
            // Write to both channels
            current_buffer[i * 2] = final_sample;     // Left channel
            current_buffer[i * 2 + 1] = final_sample; // Right channel
            
            // Debug: Log first few samples to see if we're generating audio
            if (i < 3 && melody_state.current_note_idx == 0 && melody_state.current_sample < 100) {
                ESP_LOGI(TAG, "Sample[%d]: mixed=%ld, final=%d", i, mixed_sample, final_sample);
            }
            
            melody_state.current_sample++;
            
            // Check if melody is complete
            if (melody_state.current_note_idx >= melody_state.note_count) {
                ESP_LOGI(TAG, "Melody complete: note_idx=%d >= note_count=%d", 
                         melody_state.current_note_idx, melody_state.note_count);
                melody_state.is_playing = false;
                break;
            }
        }
        
        if (!melody_state.is_playing) {
            ESP_LOGI(TAG, "Melody stopped, breaking from loop");
            break;
        }
        
        // ESP_LOGD(TAG, "About to write buffer to I2S, buffer_size=%d", buffer_size);
        
        // Write buffer to I2S
        esp_err_t ret = i2s_write(I2S_NUM_0, current_buffer, buffer_size, 
                                 &bytes_written, pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed in melody task: %s", esp_err_to_name(ret));
            break;
        }
        
        // Swap buffers
        int16_t *temp = current_buffer;
        current_buffer = next_buffer;
        next_buffer = temp;
    }
    
    ESP_LOGI(TAG, "Melody playback complete");
    
    // Stop I2S to turn off the clocks
    i2s_zero_dma_buffer(I2S_NUM_0);  // Clear DMA buffer to prevent noise
    i2s_stop(I2S_NUM_0);              // Stop I2S peripheral
    ESP_LOGI(TAG, "I2S stopped after melody playback");
    
    // Clean up
    heap_caps_free(buffer_A);
    heap_caps_free(buffer_B);
    
    // Clear melody state
    if (melody_state.notes) {
        heap_caps_free(melody_state.notes);
        melody_state.notes = NULL;
    }
    melody_state.note_count = 0;
    melody_state.is_playing = false;
    
    i2s_melody_task_handle = NULL;
    vTaskDelete(NULL);
}

// Timer callback for tone duration
static void i2s_timer_callback(void *arg)
{
    i2s_playing = false;
    if (i2s_timer_handle)
    {
        esp_timer_stop(i2s_timer_handle);
    }
    ESP_LOGD(TAG, "I2S tone duration completed");
}

// Stop tone playback and wait for its task to exit. Caller holds s_audio_mutex.
static void i2s_stop_tone_locked(void)
{
    i2s_playing = false;
    if (i2s_timer_handle)
    {
        esp_timer_stop(i2s_timer_handle);
    }
    if (!wait_for_task_exit(&i2s_tone_task_handle, 2 * I2S_WRITE_TIMEOUT_MS))
    {
        ESP_LOGE(TAG, "I2S tone task did not exit");
    }
}

// Stop melody playback and wait for its task to exit (it frees the notes).
// Caller holds s_audio_mutex.
static void i2s_stop_melody_locked(void)
{
    melody_state.is_playing = false;
    if (!wait_for_task_exit(&i2s_melody_task_handle, 2 * I2S_WRITE_TIMEOUT_MS))
    {
        ESP_LOGE(TAG, "I2S melody task did not exit");
    }
}

int32_t native_i2s_init(wasm_exec_env_t exec_env, int32_t sample_rate, int32_t bits_per_sample)
{
    if (i2s_initialized)
    {
        ESP_LOGW(TAG, "I2S already initialized");
        return 0;
    }

    // Validate parameters
    if (sample_rate != 8000 && sample_rate != 16000 && sample_rate != 22050 &&
        sample_rate != 44100 && sample_rate != 48000)
    {
        ESP_LOGE(TAG, "Unsupported sample rate: %d", sample_rate);
        return -1;
    }

    if (bits_per_sample != 16 && bits_per_sample != 32)
    {
        ESP_LOGE(TAG, "Unsupported bits per sample: %d", bits_per_sample);
        return -2;
    }

    for (int i = 0; i < 3; i++)
    {
        if (!host_pin_check(i2s_pins[i], PIN_OUTPUT, "i2s_init"))
        {
            return -7; // I2S pin not available
        }
    }

    // Initialize sine table
    esp_err_t ret = init_sine_table();
    if (ret != ESP_OK)
    {
        return -3;
    }

    gpio_set_drive_capability(GPIO_NUM_27, GPIO_DRIVE_CAP_0); // 5mA (weakest)
    gpio_set_drive_capability(GPIO_NUM_26, GPIO_DRIVE_CAP_0); // 5mA
    gpio_set_drive_capability(GPIO_NUM_25, GPIO_DRIVE_CAP_0); // 5mA

    gpio_pulldown_en(GPIO_NUM_27);
    gpio_pulldown_en(GPIO_NUM_26);
    gpio_pulldown_en(GPIO_NUM_25);

    // Configure I2S
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .tx_desc_auto_clear = true,
        .dma_desc_num = I2S_DMA_DESC_NUM,
        .dma_frame_num = I2S_DMA_FRAME_NUM,
        .use_apll = false,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1};

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE};

    // Install and start I2S driver
    ret = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S driver install failed: %s", esp_err_to_name(ret));
        return -4;
    }

    ret = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S set pin failed: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(I2S_NUM_0);
        return -5;
    }

    // Create timer for tone duration
    esp_timer_create_args_t timer_args = {
        .callback = i2s_timer_callback,
        .name = "i2s_timer"};
    ret = esp_timer_create(&timer_args, &i2s_timer_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create I2S timer: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(I2S_NUM_0);
        return -6;
    }

    i2s_sample_rate = sample_rate;
    i2s_bits_per_sample = bits_per_sample;
    i2s_initialized = true;
    i2s_playing = false;
    mcp_host_reserve_pins(i2s_pins, 3, "I2S audio");

    // Stop I2S immediately after initialization to prevent continuous clock
    // It will be started when actually playing audio
    esp_err_t stop_ret = i2s_stop(I2S_NUM_0);
    if (stop_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to stop I2S after init: %s", esp_err_to_name(stop_ret));
    }

    ESP_LOGI(TAG, "I2S initialized and stopped: %dHz, %d-bit, pins BCK=%d WS=%d DATA=%d",
             sample_rate, bits_per_sample, I2S_BCK_PIN, I2S_WS_PIN, I2S_DATA_PIN);

    return 0;
}

// Start I2S peripheral (begin clock generation)
int32_t native_i2s_start(wasm_exec_env_t exec_env)
{
    if (!i2s_initialized)
    {
        ESP_LOGE(TAG, "I2S not initialized");
        return -1;
    }

    esp_err_t ret = i2s_start(I2S_NUM_0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start I2S: %s", esp_err_to_name(ret));
        return -2;
    }

    ESP_LOGD(TAG, "I2S started");
    return 0;
}

// Stop I2S peripheral (stop clock generation)
int32_t native_i2s_stop(wasm_exec_env_t exec_env)
{
    if (!i2s_initialized)
    {
        ESP_LOGE(TAG, "I2S not initialized");
        return -1;
    }

    esp_err_t ret = i2s_stop(I2S_NUM_0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to stop I2S: %s", esp_err_to_name(ret));
        return -2;
    }

    ESP_LOGD(TAG, "I2S stopped");
    return 0;
}

int32_t native_i2s_deinit(wasm_exec_env_t exec_env)
{
    if (!i2s_initialized)
    {
        return 0;
    }

    pthread_mutex_lock(&s_audio_mutex);

    // Stop playback
    i2s_stop_tone_locked();
    i2s_stop_melody_locked();
    i2s_zero_dma_buffer(I2S_NUM_0);

    // Clean up timer
    if (i2s_timer_handle)
    {
        esp_timer_delete(i2s_timer_handle);
        i2s_timer_handle = NULL;
    }

    // Uninstall I2S driver
    i2s_driver_uninstall(I2S_NUM_0);

    // Free sine table
    if (sine_table)
    {
        mcp_free(sine_table);
        sine_table = NULL;
    }

    i2s_initialized = false;
    mcp_host_release_pins(i2s_pins, 3);
    pthread_mutex_unlock(&s_audio_mutex);
    ESP_LOGI(TAG, "I2S deinitialized");

    return 0;
}

int32_t native_i2s_play_tone(wasm_exec_env_t exec_env, int32_t frequency_hz, int32_t duration_ms, int32_t volume_percent)
{
    if (!i2s_initialized)
    {
        ESP_LOGE(TAG, "I2S not initialized");
        return -1;
    }

    // Validate parameters
    if (frequency_hz < 20 || frequency_hz > 20000)
    {
        ESP_LOGE(TAG, "Frequency out of range: %d Hz", frequency_hz);
        return -2;
    }

    if (volume_percent < 0 || volume_percent > 100)
    {
        ESP_LOGE(TAG, "Volume out of range: %d%%", volume_percent);
        return -3;
    }

    if (duration_ms < 0 || duration_ms > 60000)
    {
        ESP_LOGE(TAG, "Duration out of range: %d ms", duration_ms);
        return -4;
    }

    pthread_mutex_lock(&s_audio_mutex);

    // Stop any current playback (tone or melody) before starting a new tone
    i2s_stop_melody_locked();
    i2s_stop_tone_locked();

    // Set audio parameters
    current_frequency = (float)frequency_hz;
    i2s_volume = volume_percent;
    phase_accumulator = 0.0f;
    i2s_playing = true;

    // Initialize envelope for this note
    init_envelope_for_note(&current_envelope, duration_ms, i2s_sample_rate);

    // Create audio generation task
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        i2s_audio_task,
        "i2s_audio",
        4096,
        NULL,
        20,
        (TaskHandle_t *)&i2s_tone_task_handle,
        1);

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2S tone task, error code: %d", (int)task_ret);
        i2s_playing = false;
        i2s_tone_task_handle = NULL;
        pthread_mutex_unlock(&s_audio_mutex);
        return -5;
    }

    // Set duration timer if specified
    if (duration_ms > 0)
    {
        esp_err_t ret = esp_timer_start_once(i2s_timer_handle, duration_ms * 1000);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to start duration timer: %s", esp_err_to_name(ret));
        }
    }

    pthread_mutex_unlock(&s_audio_mutex);

    ESP_LOGI(TAG, "Playing I2S tone: %d Hz, %d ms, %d%% volume", frequency_hz, duration_ms, volume_percent);

    return 0;
}

int32_t native_i2s_stop_tone(wasm_exec_env_t exec_env)
{
    if (!i2s_initialized)
    {
        return -1;
    }

    pthread_mutex_lock(&s_audio_mutex);
    i2s_stop_tone_locked();

    // Clear I2S DMA buffer
    i2s_zero_dma_buffer(I2S_NUM_0);
    pthread_mutex_unlock(&s_audio_mutex);

    ESP_LOGI(TAG, "I2S tone stopped");

    return 0;
}

int32_t native_i2s_set_volume(wasm_exec_env_t exec_env, int32_t volume_percent)
{
    if (volume_percent < 0 || volume_percent > 100)
    {
        ESP_LOGE(TAG, "Volume out of range: %d%%", volume_percent);
        return -1;
    }

    i2s_volume = volume_percent;
    ESP_LOGI(TAG, "I2S volume set to %d%%", volume_percent);

    return 0;
}

int32_t native_i2s_is_playing(wasm_exec_env_t exec_env)
{
    return i2s_playing ? 1 : 0;
}

// Set envelope parameters for more natural sound
int32_t native_i2s_set_envelope(wasm_exec_env_t exec_env, int32_t attack_ms, int32_t release_ms, int32_t envelope_type)
{
    // Validate parameters
    if (attack_ms < 0 || attack_ms > 1000) {
        ESP_LOGE(TAG, "Invalid attack time: %d ms", attack_ms);
        return -1;
    }
    
    if (release_ms < 0 || release_ms > 2000) {
        ESP_LOGE(TAG, "Invalid release time: %d ms", release_ms);
        return -2;
    }
    
    if (envelope_type < 0 || envelope_type > 2) {
        ESP_LOGE(TAG, "Invalid envelope type: %d", envelope_type);
        return -3;
    }
    
    // Update envelope settings (will be used for next note)
    current_envelope.type = (envelope_type_t)envelope_type;
    envelope_attack_ms = attack_ms;
    envelope_release_ms = release_ms;
    
    ESP_LOGI(TAG, "Envelope set: attack=%dms, release=%dms, type=%d", 
             attack_ms, release_ms, envelope_type);
    
    return 0;
}

// Play melody with continuous audio and crossfading
int32_t native_i2s_play_melody(wasm_exec_env_t exec_env, const char *melody_str, int32_t volume_percent) {
    if (!i2s_initialized) {
        ESP_LOGE(TAG, "I2S not initialized");
        return -1;
    }
    
    if (!melody_str || strlen(melody_str) == 0) {
        ESP_LOGE(TAG, "Invalid melody string");
        return -2;
    }
    
    if (volume_percent < 0 || volume_percent > 100) {
        ESP_LOGE(TAG, "Invalid volume: %d%%", volume_percent);
        return -3;
    }
    
    // Parse the melody string
    melody_note_t *notes = NULL;
    int note_count = 0;
    
    if (parse_melody_string(melody_str, &notes, &note_count) != 0) {
        ESP_LOGE(TAG, "Failed to parse melody string");
        return -4;
    }
    
    if (note_count == 0) {
        ESP_LOGE(TAG, "No valid notes in melody");
        if (notes) heap_caps_free(notes);
        return -5;
    }
    
    pthread_mutex_lock(&s_audio_mutex);
    
    // Stop any current playback and wait until the old task has exited (and
    // freed its notes) before installing the new melody
    i2s_stop_tone_locked();
    i2s_stop_melody_locked();
    
    // Initialize melody state
    melody_state.notes = notes;
    melody_state.note_count = note_count;
    melody_state.current_note_idx = 0;
    melody_state.current_sample = 0;
    melody_state.total_samples = notes[note_count - 1].end_sample;
    melody_state.is_playing = true;
    melody_state.crossfading = false;
    melody_state.osc_a.active = false;
    melody_state.osc_b.active = false;
    
    // Set volume
    i2s_volume = volume_percent;
    
    ESP_LOGI(TAG, "Playing melody: %d notes, volume %d%%", note_count, volume_percent);
    
    // Create melody playback task
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        i2s_melody_task,
        "i2s_melody",
        3072,   // logs floats; 2KB was tight
        NULL,
        20,
        (TaskHandle_t *)&i2s_melody_task_handle,
        1);
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2S melody task, error code: %d", (int)task_ret);
        // Additionally, let's check available memory:
        ESP_LOGE(TAG, "Free heap: %d bytes", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
        ESP_LOGE(TAG, "Largest free block: %d bytes", heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        ESP_LOGE(TAG, "Current task count: %d", uxTaskGetNumberOfTasks());
        ESP_LOGE(TAG, "Internal DRAM free: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGE(TAG, "Internal DRAM largest block: %d bytes", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

        heap_caps_free(notes);
        melody_state.notes = NULL;
        melody_state.is_playing = false;
        i2s_melody_task_handle = NULL;
        pthread_mutex_unlock(&s_audio_mutex);
        return -6;
    }
    
    pthread_mutex_unlock(&s_audio_mutex);
    return 0;
}

// Stop melody playback
int32_t native_i2s_stop_melody(wasm_exec_env_t exec_env) {
    if (!i2s_initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&s_audio_mutex);
    i2s_stop_melody_locked();
    pthread_mutex_unlock(&s_audio_mutex);
    
    ESP_LOGI(TAG, "Melody playback stopped");
    
    return 0;
}

// ========== Host Function Registration ==========

NativeSymbol mcp_core_host_functions[] = {
    // Memory management
    {"malloc", mcp_native_malloc, "(i)i", NULL},
    {"free", mcp_native_free, "(i)", NULL},

    // String functions - using automatic string conversion ($)
    // {"strlen", mcp_native_strlen, "($)i", NULL},
    // {"strcmp", mcp_native_strcmp, "($$)i", NULL},
    // {"strcpy", mcp_native_strcpy, "($$)i", NULL},
    {"strcat", mcp_native_strcat, "($$)i", NULL},
    // {"strncpy", mcp_native_strncpy, "($*i)i", NULL},
    // {"strstr", mcp_native_strstr, "($$)i", NULL},
    // {"strncmp", mcp_native_strncmp, "($$i)i", NULL},

    // {"atoi", mcp_native_atoi, "($)i", NULL},

    // Memory functions
    // {"memcpy", mcp_native_memcpy, "(**i)i", NULL},
    // {"memcmp", mcp_native_memcmp, "(**i)i", NULL},

    // Logging functions
    {"log_info", mcp_native_log_info, "($$)i", NULL},
    {"log_error", mcp_native_log_error, "($$)i", NULL},

    // System information
    {"get_free_heap", mcp_native_get_free_heap, "()i", NULL},
    {"get_uptime_ms", mcp_native_get_uptime_ms, "()i", NULL},
    {"get_tick_count", mcp_native_get_tick_count, "()i", NULL},

    // GPIO Control (ESP32-specific but reusable)
    {"gpio_set_level", native_gpio_set_level, "(ii)i", NULL},
    {"gpio_get_level", native_gpio_get_level, "(i)i", NULL},
    {"gpio_set_direction", native_gpio_set_direction, "(ii)i", NULL},
    {"gpio_is_pin_protected", native_gpio_is_pin_protected, "(i)i", NULL},

    // PWM Control (LEDC-based for music/speaker control)
    {"pwm_init", native_pwm_init, "(ii)i", NULL},
    {"pwm_set_duty", native_pwm_set_duty, "(ii)i", NULL},
    {"pwm_stop", native_pwm_stop, "(i)i", NULL},
    {"pwm_set_frequency", native_pwm_set_frequency, "(ii)i", NULL},

    // I2S Audio Control (for high-quality audio output)
    {"i2s_init", native_i2s_init, "(ii)i", NULL},
    {"i2s_deinit", native_i2s_deinit, "()i", NULL},
    {"i2s_start", native_i2s_start, "()i", NULL},
    {"i2s_stop", native_i2s_stop, "()i", NULL},
    {"i2s_play_tone", native_i2s_play_tone, "(iii)i", NULL},
    {"i2s_stop_tone", native_i2s_stop_tone, "()i", NULL},
    {"i2s_set_volume", native_i2s_set_volume, "(i)i", NULL},
    {"i2s_is_playing", native_i2s_is_playing, "()i", NULL},
    {"i2s_set_envelope", native_i2s_set_envelope, "(iii)i", NULL},
    {"i2s_play_melody", native_i2s_play_melody, "($i)i", NULL},
    {"i2s_stop_melody", native_i2s_stop_melody, "()i", NULL},

    // Bluetooth Audio Control (A2DP sink for wireless speakers/headphones)
    {"bluetooth_init", bluetooth_init, "()i", NULL},
    {"bluetooth_deinit", bluetooth_deinit, "()i", NULL},
    {"bluetooth_scan_start", bluetooth_scan_start, "(i)i", NULL},
    {"bluetooth_scan_get_results", bluetooth_scan_get_results, "(*~)i", NULL},
    {"bluetooth_pair_device", bluetooth_pair_device, "($)i", NULL},
    {"bluetooth_connect_device", bluetooth_connect_device, "($)i", NULL},
    {"bluetooth_disconnect", bluetooth_disconnect, "()i", NULL},
    {"bluetooth_is_connected", bluetooth_is_connected, "()i", NULL},
    {"bluetooth_get_connected_device_name", bluetooth_get_connected_device_name, "(*~)i", NULL},
    {"bluetooth_get_connected_device_address", bluetooth_get_connected_device_address, "(*~)i", NULL},
    {"bluetooth_play_tone", bluetooth_play_tone, "(iii)i", NULL},
    {"bluetooth_play_melody", bluetooth_play_melody, "($i)i", NULL},
    {"bluetooth_set_volume", bluetooth_set_volume, "(i)i", NULL},

    // SSE Events (MCP Protocol)
    {"sse_send_event", native_sse_send_event, "($$)i", NULL},
    {"sse_send_tool_result", native_sse_send_tool_result, "($$)i", NULL},

    // JSON-RPC Protocol (MCP Framework)
    {"json_parse_request", native_json_parse_request, "($**i)i", NULL},
    {"json_create_response", native_json_create_response, "($*~)i", NULL},
    {"json_create_error", native_json_create_error, "(i$*~)i", NULL},

    // HTTP Header Management (using buffer+length pattern)
    {"set_response_header", mcp_native_set_response_header, "($$)i", NULL},
    {"get_request_header", mcp_native_get_request_header, "($*~)i", NULL},
    {"remove_response_header", mcp_native_remove_response_header, "($)i", NULL},
    {"list_request_headers", mcp_native_list_request_headers, "(*~)i", NULL},
};

const int mcp_core_host_functions_count = sizeof(mcp_core_host_functions) / sizeof(NativeSymbol);