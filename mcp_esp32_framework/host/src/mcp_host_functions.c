#include "mcp_host_functions.h"
#include "mcp_memory.h"
#include "mcp_config_loader.h"
#include "mcp_session_manager.h"
#include "mcp_pthread_pool.h"
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

static const char* TAG = "MCP_HOST";

// WASM memory alignment size
#define MALLOC_ALIGN_SIZE 8

// Thread-local storage for current request context (used by header functions)
static _Thread_local mcp_request_t* tls_current_request = NULL;

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
    if (new_ptr) {
        if (ptr) {
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

uint32_t mcp_native_malloc(wasm_exec_env_t exec_env, uint32_t size) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    if (size == 0 || size > 64 * 1024) { // Reasonable size limit
        ESP_LOGE(TAG, "Invalid malloc size: %u", size);
        return 0;
    }
    
    uint32_t wasm_addr = wasm_runtime_module_malloc(module_inst, size, NULL);
    if (wasm_addr == 0) {
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

void mcp_native_free(wasm_exec_env_t exec_env, uint32_t ptr) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    if (ptr == 0) {
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

int32_t mcp_native_strlen(wasm_exec_env_t exec_env, const char* s) {
    if (!s) {
        return 0;
    }
    return (int32_t)strlen(s);
}

int32_t mcp_native_strcmp(wasm_exec_env_t exec_env, const char* s1, const char* s2) {
    if (!s1 || !s2) {
        return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    }
    return strcmp(s1, s2);
}

int32_t mcp_native_strcpy(wasm_exec_env_t exec_env, char* dest, const char* src) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    if (!dest || !src) {
        return 0;
    }
    
    strcpy(dest, src);
    
    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, dest);
    return wasm_addr;
}

int32_t mcp_native_strcat(wasm_exec_env_t exec_env, char* dest, const char* src) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    if (!dest || !src) {
        return 0;
    }
    
    strcat(dest, src);
    
    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, dest);
    return wasm_addr;
}

int32_t mcp_native_strncpy(wasm_exec_env_t exec_env, char* dest, const char* src, int32_t n) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    if (!dest || !src || n <= 0) {
        return 0;
    }
    
    strncpy(dest, src, n);
    
    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, dest);
    return wasm_addr;
}

int32_t mcp_native_strstr(wasm_exec_env_t exec_env, const char* haystack, const char* needle) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    if (!haystack || !needle) {
        return 0;
    }
    
    char* result = strstr(haystack, needle);
    if (!result) {
        return 0;
    }
    
    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, result);
    return wasm_addr;
}

int32_t mcp_native_atoi(wasm_exec_env_t exec_env, const char* str) {
    if (!str) {
        return 0;
    }
    
    return atoi(str);
}

// ========== Memory Functions ==========

int32_t mcp_native_memcpy(wasm_exec_env_t exec_env, void* dest, const void* src, int32_t n) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    if (!dest || !src || n <= 0) {
        return 0;
    }
    
    memcpy(dest, src, n);
    
    // Convert host pointer back to WASM address
    uint32_t wasm_addr = wasm_runtime_addr_native_to_app(module_inst, dest);
    return wasm_addr;
}

int32_t mcp_native_memcmp(wasm_exec_env_t exec_env, const void* s1, const void* s2, int32_t n) {
    // Using automatic pointer conversion (**i) - no validation needed
    return memcmp(s1, s2, n);
}

int32_t mcp_native_strncmp(wasm_exec_env_t exec_env, const char* s1, const char* s2, int32_t n) {
    // Using automatic string conversion ($$) - no validation needed
    return strncmp(s1, s2, n);
}

// ========== Logging Functions ==========

int32_t mcp_native_log_info(wasm_exec_env_t exec_env, const char* tag, const char* message) {
    if (!tag || !message) {
        ESP_LOGE(TAG, "LOG_INFO: Invalid parameters - tag=%p, message=%p", tag, message);
        return -1;
    }
    
    // Limit string lengths for safety
    size_t tag_len = strnlen(tag, 64);
    size_t msg_len = strnlen(message, 512);
    
    if (tag_len == 0 || msg_len == 0) {
        ESP_LOGW(TAG, "LOG_INFO: Empty string detected - tag_len=%zu, msg_len=%zu", tag_len, msg_len);
        return -1;
    }
    
    ESP_LOGI(tag, "[WASM] %s", message);
    return 0;
}

int32_t mcp_native_log_error(wasm_exec_env_t exec_env, const char* tag, const char* message) {
    if (!tag || !message) {
        ESP_LOGE(TAG, "LOG_ERROR: Invalid parameters - tag=%p, message=%p", tag, message);
        return -1;
    }
    
    // Limit string lengths for safety
    size_t tag_len = strnlen(tag, 64);
    size_t msg_len = strnlen(message, 512);
    
    if (tag_len == 0 || msg_len == 0) {
        ESP_LOGW(TAG, "LOG_ERROR: Empty string detected - tag_len=%zu, msg_len=%zu", tag_len, msg_len);
        return -1;
    }
    
    ESP_LOGE(tag, "[WASM] %s", message);
    return 0;
}

// ========== System Information Functions ==========

int32_t mcp_native_get_free_heap(wasm_exec_env_t exec_env) {
    return (int32_t)esp_get_free_heap_size();
}

int32_t mcp_native_get_uptime_ms(wasm_exec_env_t exec_env) {
    return (int32_t)(esp_timer_get_time() / 1000);
}

int32_t mcp_native_get_tick_count(wasm_exec_env_t exec_env) {
    return (int32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// ========== GPIO Functions (ESP32-specific) ==========

int32_t native_gpio_set_level(wasm_exec_env_t exec_env, int32_t pin, int32_t level)
{
    // Check if pin is protected
    if (pin >= 0 && pin < 40) {
        for (int i = 0; i < g_mcp_config.gpio_protection.protected_pin_count; i++) {
            if (g_mcp_config.gpio_protection.protected_pins[i] == pin) {
                ESP_LOGW(TAG, "GPIO pin %d is protected, access denied", pin);
                return -1; // Protected pin
            }
        }
    }

    if (pin < 0 || pin >= 40) {
        return -1; // Invalid pin
    }

    esp_err_t result = gpio_set_level((gpio_num_t)pin, level);
    return (result == ESP_OK) ? 0 : -1;
}

int32_t native_gpio_get_level(wasm_exec_env_t exec_env, int32_t pin)
{
    if (pin < 0 || pin >= 40) {
        return -1; // Invalid pin
    }
    
    return gpio_get_level((gpio_num_t)pin);
}

int32_t native_gpio_set_direction(wasm_exec_env_t exec_env, int32_t pin, int32_t direction)
{
    // Check if pin is protected
    if (pin >= 0 && pin < 40) {
        for (int i = 0; i < g_mcp_config.gpio_protection.protected_pin_count; i++) {
            if (g_mcp_config.gpio_protection.protected_pins[i] == pin) {
                ESP_LOGW(TAG, "GPIO pin %d is protected, direction change denied", pin);
                return -1;
            }
        }
    }

    if (pin < 0 || pin >= 40) {
        return -1; // Invalid pin
    }

    gpio_mode_t mode;
    switch (direction) {
        case 0: mode = GPIO_MODE_INPUT; break;
        case 1: mode = GPIO_MODE_OUTPUT; break;
        case 2: mode = GPIO_MODE_INPUT_OUTPUT; break;
        default: return -1; // Invalid direction
    }

    esp_err_t result = gpio_set_direction((gpio_num_t)pin, mode);
    return (result == ESP_OK) ? 0 : -1;
}

int32_t native_gpio_is_pin_protected(wasm_exec_env_t exec_env, int32_t pin)
{
    for (int i = 0; i < g_mcp_config.gpio_protection.protected_pin_count; i++) {
        if (g_mcp_config.gpio_protection.protected_pins[i] == pin) {
            return 1; // Protected
        }
    }
    return 0; // Not protected
}

// ========== PWM Functions (LEDC-based for Music/Speaker Control) ==========

#define PWM_TIMER_RESOLUTION    LEDC_TIMER_12_BIT
#define PWM_MODE               LEDC_LOW_SPEED_MODE
#define PWM_TIMER              LEDC_TIMER_0
#define PWM_CHANNEL            LEDC_CHANNEL_0
#define PWM_DUTY_MAX           ((1 << PWM_TIMER_RESOLUTION) - 1)  // 4095 for 12-bit

static bool pwm_initialized = false;
static int32_t pwm_current_pin = -1;

int32_t native_pwm_init(wasm_exec_env_t exec_env, int32_t pin, int32_t frequency)
{
    // Validate parameters
    if (pin < 0 || pin >= 40) {
        ESP_LOGE(TAG, "PWM: Invalid pin %d", pin);
        return -1;
    }
    
    if (frequency < 1 || frequency > 40000) {  // 1Hz to 40kHz range
        ESP_LOGE(TAG, "PWM: Invalid frequency %d Hz", frequency);
        return -2;
    }
    
    // Check if pin is protected
    if (pin >= 0 && pin < 40) {
        for (int i = 0; i < g_mcp_config.gpio_protection.protected_pin_count; i++) {
            if (g_mcp_config.gpio_protection.protected_pins[i] == pin) {
                ESP_LOGW(TAG, "PWM: Pin %d is protected, access denied", pin);
                return -3;
            }
        }
    }
    
    esp_err_t err;
    
    // Configure timer
    ledc_timer_config_t timer_config = {
        .duty_resolution = PWM_TIMER_RESOLUTION,
        .freq_hz = frequency,
        .speed_mode = PWM_MODE,
        .timer_num = PWM_TIMER,
        .clk_cfg = LEDC_AUTO_CLK
    };
    
    err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWM: Timer config failed: %s", esp_err_to_name(err));
        return -4;
    }
    
    // Configure channel
    ledc_channel_config_t channel_config = {
        .channel = PWM_CHANNEL,
        .duty = 0,  // Start with 0% duty
        .gpio_num = pin,
        .speed_mode = PWM_MODE,
        .hpoint = 0,
        .timer_sel = PWM_TIMER
    };
    
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWM: Channel config failed: %s", esp_err_to_name(err));
        return -5;
    }
    
    pwm_initialized = true;
    pwm_current_pin = pin;
    
    ESP_LOGI(TAG, "PWM: Initialized pin %d at %d Hz", pin, frequency);
    return 0;
}

int32_t native_pwm_set_duty(wasm_exec_env_t exec_env, int32_t pin, int32_t duty_percent)
{
    if (!pwm_initialized || pwm_current_pin != pin) {
        ESP_LOGE(TAG, "PWM: Not initialized for pin %d", pin);
        return -1;
    }
    
    if (duty_percent < 0 || duty_percent > 100) {
        ESP_LOGE(TAG, "PWM: Invalid duty cycle %d%% (0-100 required)", duty_percent);
        return -2;
    }
    
    // Convert percentage to duty value (0-4095 for 12-bit)
    uint32_t duty = (duty_percent * PWM_DUTY_MAX) / 100;
    
    esp_err_t err = ledc_set_duty(PWM_MODE, PWM_CHANNEL, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWM: Set duty failed: %s", esp_err_to_name(err));
        return -3;
    }
    
    err = ledc_update_duty(PWM_MODE, PWM_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWM: Update duty failed: %s", esp_err_to_name(err));
        return -4;
    }
    
    ESP_LOGD(TAG, "PWM: Pin %d duty set to %d%% (raw: %lu)", pin, duty_percent, duty);
    return 0;
}

int32_t native_pwm_stop(wasm_exec_env_t exec_env, int32_t pin)
{
    if (!pwm_initialized || pwm_current_pin != pin) {
        ESP_LOGW(TAG, "PWM: Stop called but not initialized for pin %d", pin);
        return 0;  // Not an error, just no-op
    }
    
    // Set duty to 0 to stop output
    esp_err_t err = ledc_set_duty(PWM_MODE, PWM_CHANNEL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWM: Stop duty set failed: %s", esp_err_to_name(err));
        return -1;
    }
    
    err = ledc_update_duty(PWM_MODE, PWM_CHANNEL);
    if (err != ESP_OK) {
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
    if (!pwm_initialized || pwm_current_pin != pin) {
        ESP_LOGE(TAG, "PWM: Not initialized for pin %d", pin);
        return -1;
    }
    
    if (frequency < 1 || frequency > 40000) {
        ESP_LOGE(TAG, "PWM: Invalid frequency %d Hz", frequency);
        return -2;
    }
    
    // Update timer frequency
    esp_err_t err = ledc_set_freq(PWM_MODE, PWM_TIMER, frequency);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWM: Set frequency failed: %s", esp_err_to_name(err));
        return -3;
    }
    
    ESP_LOGD(TAG, "PWM: Pin %d frequency updated to %d Hz", pin, frequency);
    return 0;
}

// ========== WAMR Memory Allocator Access Functions ==========

void mcp_get_wamr_allocators(void** malloc_func, void** realloc_func, void** free_func) {
    if (malloc_func) *malloc_func = (void*)wamr_malloc;
    if (realloc_func) *realloc_func = (void*)wamr_realloc;  
    if (free_func) *free_func = (void*)wamr_free;
}

// ========== SSE Event Functions (MCP Protocol) ==========

int32_t native_sse_send_event(wasm_exec_env_t exec_env, const char* event_type, const char* data)
{
    if (!event_type || !data) {
        ESP_LOGE(TAG, "SSE_SEND_EVENT: Invalid parameters - event_type=%p, data=%p", event_type, data);
        return -1;
    }
    
    ESP_LOGD(TAG, "WASM sending SSE event: %s = %s", event_type, data);
    
    // Send event to current session using session manager
    esp_err_t ret = mcp_session_send_to_current(event_type, data);
    
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "SSE event sent successfully");
        return 0;
    } else if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No current session or SSE connection for event");
        return -2;
    } else {
        ESP_LOGE(TAG, "Failed to send SSE event: %s", esp_err_to_name(ret));
        return -1;
    }
}

int32_t native_sse_send_tool_result(wasm_exec_env_t exec_env, const char* tool_name, const char* result)
{
    if (!tool_name || !result) {
        ESP_LOGE(TAG, "SSE_SEND_TOOL_RESULT: Invalid parameters - tool_name=%p, result=%p", tool_name, result);
        return -1;
    }
    
    // Create JSON-formatted tool result
    char event_data[512];
    int len = snprintf(event_data, sizeof(event_data), "{\"tool\":\"%s\",\"result\":%s}", tool_name, result);
    
    if (len >= sizeof(event_data)) {
        ESP_LOGE(TAG, "Tool result too large for buffer (%d >= %zu)", len, sizeof(event_data));
        return -3;
    }
    
    ESP_LOGD(TAG, "WASM tool result: %s", event_data);
    
    // Send tool result event to current session
    esp_err_t ret = mcp_session_send_to_current("tool_result", event_data);
    
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Tool result sent successfully");
        return 0;
    } else if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No current session or SSE connection for tool result");
        return -2;
    } else {
        ESP_LOGE(TAG, "Failed to send tool result: %s", esp_err_to_name(ret));
        return -1;
    }
}

// ========== JSON-RPC Protocol Functions (MCP Framework) ==========

int32_t native_json_parse_request(wasm_exec_env_t exec_env, const char* json_str, 
                                         char* method_out, char* params_out, int32_t max_len)
{
    ESP_LOGD(TAG, "JSON_PARSE: Called with json=%p, method=%p, params=%p, max_len=%d", json_str, method_out, params_out, max_len);
     wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    
    
    
    // Since we're using automatic conversion ($**i), the pointers are already native pointers
    // No validation needed - just direct access
    
    ESP_LOGD(TAG, "JSON_PARSE: All validations passed, JSON content: '%.50s'", json_str);
    
    // Parse JSON-RPC method
    ESP_LOGD(TAG, "JSON_PARSE: About to call json_find_string");
    if (!mcp_json_find_string(json_str, "method", method_out, 64)) {  // method buffer is 64 bytes
        ESP_LOGE(TAG, "JSON_PARSE: json_find_string failed - method not found");
        return -2; // Method not found
    }

    // Convert host pointer back to WASM address
    //uint32_t method_out = wasm_runtime_addr_native_to_app(module_inst, method);
    
    ESP_LOGD(TAG, "JSON_PARSE: Successfully parsed method: '%s'", method_out);
    
    // Parse JSON-RPC params (extract the params object as string)
    const char* params_start = strstr(json_str, "\"params\"");
    if (params_start) {
        params_start = strchr(params_start, ':');
        if (params_start) {
            params_start++; // Skip ':'
            while (*params_start == ' ' || *params_start == '\t') params_start++; // Skip whitespace
            
            // Copy the params value
            int i = 0;
            int brace_depth = 0;
            bool in_string = false;
            bool escape_next = false;
            
            while (params_start[i] && i < max_len - 1) {
                char c = params_start[i];
                
                if (escape_next) {
                    escape_next = false;
                } else if (c == '\\') {
                    escape_next = true;
                } else if (c == '"' && !escape_next) {
                    in_string = !in_string;
                } else if (!in_string) {
                    if (c == '{') brace_depth++;
                    else if (c == '}') brace_depth--;
                    else if ((c == ',' || c == '}') && brace_depth == 0) break;
                }
                
                params_out[i] = c;
                i++;
                
                if (!in_string && brace_depth == 0 && (c == ']' || c == '"' || (c >= '0' && c <= '9'))) {
                    // End of simple value
                    i++;
                    break;
                }
            }
            params_out[i] = '\0';
        } else {
            strcpy(params_out, "null");
        }
    } else {
        strcpy(params_out, "null");
    }
    
    return 0;
}

int32_t native_json_create_response(wasm_exec_env_t exec_env, const char* result, char* response, int32_t max_len)
{
    // Using buffer + length signature ($*~) - safer parameter handling
    if (!response || max_len <= 0) {
        printf("ERROR: Invalid response buffer parameters\\n");
        return -1;
    }
    
    int len = snprintf(response, max_len,
        "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":1}",
        result);
    
    printf("JSON_CREATE_RESPONSE: Created response (%d bytes): %.100s...\\n", len, response);
    return len < max_len ? 0 : -2; // Buffer overflow
}

int32_t native_json_create_error(wasm_exec_env_t exec_env, int32_t code, const char* message, char* response, int32_t max_len)
{
    // Using buffer + length signature (i$*~) - safer parameter handling
    if (!response || max_len <= 0) {
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

void mcp_host_set_request_context(void* request) {
    tls_current_request = (mcp_request_t*)request;
}

void mcp_host_clear_request_context(void) {
    tls_current_request = NULL;
}

// ========== HTTP Header Management Functions ==========


// Helper function to ensure response headers buffer has enough capacity
static int ensure_response_headers_capacity(mcp_request_t* request, size_t needed) {
    if (!request->response_headers) {
        // Initial allocation
        request->response_headers_capacity = needed > 1024 ? needed : 1024;
        request->response_headers = heap_caps_malloc(request->response_headers_capacity, MALLOC_CAP_SPIRAM);
        if (!request->response_headers) return -1;
        request->response_headers[0] = '\0';
        request->response_headers_length = 0;
    } else if (request->response_headers_length + needed >= request->response_headers_capacity) {
        // Realloc to larger size
        size_t new_capacity = (request->response_headers_capacity + needed) * 2;
        char* new_buffer = heap_caps_realloc(request->response_headers, new_capacity, MALLOC_CAP_SPIRAM);
        if (!new_buffer) return -1;
        request->response_headers = new_buffer;
        request->response_headers_capacity = new_capacity;
    }
    return 0;
}

int32_t mcp_native_set_response_header(wasm_exec_env_t exec_env, const char* name, const char* value) {
    if (!tls_current_request) {
        ESP_LOGE(TAG, "set_response_header: No current request context");
        return -1;
    }
    
    if (!name || !value) {
        ESP_LOGE(TAG, "set_response_header: Invalid parameters");
        return -1;
    }
    
    // Validate header name (basic ASCII check)
    for (const char* p = name; *p; p++) {
        if (*p < 0x21 || *p > 0x7E || *p == ':') {
            ESP_LOGE(TAG, "set_response_header: Invalid header name");
            return -1;
        }
    }
    
    // Calculate needed space: "Name: Value\r\n"
    size_t needed = strlen(name) + strlen(value) + 4;  // ": " + "\r\n"
    
    if (ensure_response_headers_capacity(tls_current_request, needed) != 0) {
        ESP_LOGE(TAG, "set_response_header: Memory allocation failed");
        return -2;
    }
    
    // Check if header already exists and remove it first
    mcp_native_remove_response_header(exec_env, name);
    
    // Append new header
    int len = snprintf(tls_current_request->response_headers + tls_current_request->response_headers_length,
                      tls_current_request->response_headers_capacity - tls_current_request->response_headers_length,
                      "%s: %s\r\n", name, value);
    
    if (len > 0) {
        tls_current_request->response_headers_length += len;
        ESP_LOGD(TAG, "set_response_header: %s = %s", name, value);
        return 0;
    }
    
    return -3;
}

// Get request header value (case-insensitive) - returns length or -1 if not found
int32_t mcp_native_get_request_header(wasm_exec_env_t exec_env, const char* name, char* buffer, int32_t buffer_size) {
    if (!tls_current_request) {
        ESP_LOGW(TAG, "get_request_header: No current request context");
        return -1;
    }

    if (!name || !buffer || buffer_size <= 0) {
        ESP_LOGE(TAG, "get_request_header: Invalid parameters");
        return -1;
    }
    
    if (!tls_current_request->headers) {
        return -1;  // No headers
    }
    
    // Inline header search - no static buffers!
    size_t name_len = strlen(name);
    const char* pos = tls_current_request->headers;
    
    while (*pos) {
        // Find header name (before colon)
        const char* colon = strchr(pos, ':');
        if (!colon) break;
        
        // Check if header name matches (case-insensitive)
        if ((colon - pos) == name_len && strncasecmp(pos, name, name_len) == 0) {
            // Found header, skip colon and whitespace
            const char* value = colon + 1;
            while (*value == ' ' || *value == '\t') value++;
            
            // Find end of value (before \r\n)
            const char* end = strstr(value, "\r\n");
            if (!end) end = value + strlen(value);
            
            // Copy directly to provided buffer - no static allocation!
            size_t value_len = end - value;
            if (value_len >= buffer_size) value_len = buffer_size - 1;
            
            strncpy(buffer, value, value_len);
            buffer[value_len] = '\0';
            
            ESP_LOGD(TAG, "get_request_header: %s = %s", name, buffer);
            return strlen(buffer);
        }
        
        // Move to next line
        const char* next_line = strstr(pos, "\r\n");
        if (!next_line) break;
        pos = next_line + 2;
    }
    
    ESP_LOGD(TAG, "get_request_header: %s not found", name);
    return -1;  // Not found
}

int32_t mcp_native_remove_response_header(wasm_exec_env_t exec_env, const char* name) {
    if (!tls_current_request || !tls_current_request->response_headers) {
        return -1;
    }
    
    if (!name) {
        return -1;
    }
    
    char* headers = tls_current_request->response_headers;
    size_t name_len = strlen(name);
    char* pos = headers;
    
    while (*pos) {
        // Find header name
        char* colon = strchr(pos, ':');
        if (!colon) break;
        
        // Check if this is the header to remove
        if ((colon - pos) == name_len && strncasecmp(pos, name, name_len) == 0) {
            // Found header to remove, find end of line
            char* line_end = strstr(pos, "\r\n");
            if (line_end) {
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
        char* next_line = strstr(pos, "\r\n");
        if (!next_line) break;
        pos = next_line + 2;
    }
    
    return -1; // Header not found
}

// List all request headers as comma-separated names - returns length or -1 if error
int32_t mcp_native_list_request_headers(wasm_exec_env_t exec_env, char* buffer, int32_t buffer_size) {
    if (!buffer || buffer_size <= 0) {
        return -1;
    }
    
    if (!tls_current_request || !tls_current_request->headers) {
        buffer[0] = '\0';
        return 0;  // Empty list
    }
    
    buffer[0] = '\0';
    const char* pos = tls_current_request->headers;
    size_t list_len = 0;
    
    while (*pos && list_len < buffer_size - 50) {
        // Find header name (before colon)
        const char* colon = strchr(pos, ':');
        if (!colon) break;
        
        // Add comma if not first header
        if (list_len > 0 && list_len < buffer_size - 1) {
            strcat(buffer, ",");
            list_len++;
        }
        
        // Add header name
        size_t name_len = colon - pos;
        if (list_len + name_len < buffer_size - 1) {
            strncat(buffer, pos, name_len);
            list_len += name_len;
        }
        
        // Move to next line
        const char* next_line = strstr(pos, "\r\n");
        if (!next_line) break;
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
static TaskHandle_t i2s_task_handle = NULL;

// I2S pin configuration (FreenoVE Audio Converter & Amplifier)
#define I2S_BCK_PIN 27   // Bit Clock
#define I2S_WS_PIN  26   // Word Select (LRC)
#define I2S_DATA_PIN 25  // Data Input (DIN)

// Audio generation parameters
#define I2S_SAMPLE_BUFFER_SIZE 512
#define I2S_DMA_DESC_NUM 8
#define I2S_DMA_FRAME_NUM 64
#define SINE_TABLE_SIZE 1024

// Pre-computed sine wave table for efficiency
static int16_t *sine_table = NULL;
static float current_frequency = 0.0f;
static float phase_accumulator = 0.0f;

// Initialize sine wave lookup table
static esp_err_t init_sine_table(void) {
    if (sine_table != NULL) return ESP_OK;
    
    // Use smart allocation - sine table can go in PSRAM for memory efficiency
    sine_table = (int16_t*)mcp_malloc(SINE_TABLE_SIZE * sizeof(int16_t));
    if (!sine_table) {
        ESP_LOGE(TAG, "Failed to allocate sine table");
        return ESP_ERR_NO_MEM;
    }
    
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        float angle = 2.0f * M_PI * i / SINE_TABLE_SIZE;
        sine_table[i] = (int16_t)(sin(angle) * 32767);
    }
    
    ESP_LOGI(TAG, "Sine table initialized (%d samples)", SINE_TABLE_SIZE);
    return ESP_OK;
}

// Audio generation task
static void i2s_audio_task(void *pvParameters) {
    // I2S DMA requires internal memory that's DMA-capable
    size_t buffer_size = I2S_SAMPLE_BUFFER_SIZE * sizeof(int16_t) * 2;
    int16_t *sample_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!sample_buffer) {
        ESP_LOGE(TAG, "Failed to allocate I2S sample buffer");
        vTaskDelete(NULL);
        return;
    }
    
    size_t bytes_written;
    float phase_increment = (current_frequency * SINE_TABLE_SIZE) / i2s_sample_rate;
    
    while (i2s_playing) {
        // Generate samples
        for (int i = 0; i < I2S_SAMPLE_BUFFER_SIZE; i++) {
            int table_index = (int)phase_accumulator % SINE_TABLE_SIZE;
            int16_t sample = (sine_table[table_index] * i2s_volume) / 100;
            
            // Stereo output (left and right channels)
            sample_buffer[i * 2] = sample;     // Left channel
            sample_buffer[i * 2 + 1] = sample; // Right channel
            
            phase_accumulator += phase_increment;
            if (phase_accumulator >= SINE_TABLE_SIZE) {
                phase_accumulator -= SINE_TABLE_SIZE;
            }
        }
        
        // Write to I2S
        esp_err_t ret = i2s_write(I2S_NUM_0, sample_buffer, 
                                  I2S_SAMPLE_BUFFER_SIZE * sizeof(int16_t) * 2, 
                                  &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            break;
        }
    }
    
    heap_caps_free(sample_buffer);
    i2s_task_handle = NULL;
    vTaskDelete(NULL);
}

// Timer callback for tone duration
static void i2s_timer_callback(void *arg) {
    i2s_playing = false;
    if (i2s_timer_handle) {
        esp_timer_stop(i2s_timer_handle);
    }
    ESP_LOGD(TAG, "I2S tone duration completed");
}

int32_t native_i2s_init(wasm_exec_env_t exec_env, int32_t sample_rate, int32_t bits_per_sample) {
    if (i2s_initialized) {
        ESP_LOGW(TAG, "I2S already initialized");
        return 0;
    }
    
    // Validate parameters
    if (sample_rate != 8000 && sample_rate != 16000 && sample_rate != 22050 && 
        sample_rate != 44100 && sample_rate != 48000) {
        ESP_LOGE(TAG, "Unsupported sample rate: %d", sample_rate);
        return -1;
    }
    
    if (bits_per_sample != 16 && bits_per_sample != 32) {
        ESP_LOGE(TAG, "Unsupported bits per sample: %d", bits_per_sample);
        return -2;
    }
    
    // Initialize sine table
    esp_err_t ret = init_sine_table();
    if (ret != ESP_OK) {
        return -3;
    }

    gpio_set_drive_capability(GPIO_NUM_27, GPIO_DRIVE_CAP_0);  // 5mA (weakest)
    gpio_set_drive_capability(GPIO_NUM_26, GPIO_DRIVE_CAP_0);  // 5mA
    gpio_set_drive_capability(GPIO_NUM_25, GPIO_DRIVE_CAP_0);  // 5mA

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
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1
    };
    
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    
    // Install and start I2S driver
    ret = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S driver install failed: %s", esp_err_to_name(ret));
        return -4;
    }
    
    ret = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S set pin failed: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(I2S_NUM_0);
        return -5;
    }
    
    // Create timer for tone duration
    esp_timer_create_args_t timer_args = {
        .callback = i2s_timer_callback,
        .name = "i2s_timer"
    };
    ret = esp_timer_create(&timer_args, &i2s_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S timer: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(I2S_NUM_0);
        return -6;
    }
    
    i2s_sample_rate = sample_rate;
    i2s_bits_per_sample = bits_per_sample;
    i2s_initialized = true;
    i2s_playing = false;
    
    // Stop I2S immediately after initialization to prevent continuous clock
    // It will be started when actually playing audio
    esp_err_t stop_ret = i2s_stop(I2S_NUM_0);
    if (stop_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop I2S after init: %s", esp_err_to_name(stop_ret));
    }
    
    ESP_LOGI(TAG, "I2S initialized and stopped: %dHz, %d-bit, pins BCK=%d WS=%d DATA=%d", 
             sample_rate, bits_per_sample, I2S_BCK_PIN, I2S_WS_PIN, I2S_DATA_PIN);
    
    return 0;
}

// Start I2S peripheral (begin clock generation)
int32_t native_i2s_start(wasm_exec_env_t exec_env) {
    if (!i2s_initialized) {
        ESP_LOGE(TAG, "I2S not initialized");
        return -1;
    }
    
    esp_err_t ret = i2s_start(I2S_NUM_0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start I2S: %s", esp_err_to_name(ret));
        return -2;
    }
    
    ESP_LOGD(TAG, "I2S started");
    return 0;
}

// Stop I2S peripheral (stop clock generation)  
int32_t native_i2s_stop(wasm_exec_env_t exec_env) {
    if (!i2s_initialized) {
        ESP_LOGE(TAG, "I2S not initialized");
        return -1;
    }
    
    esp_err_t ret = i2s_stop(I2S_NUM_0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop I2S: %s", esp_err_to_name(ret));
        return -2;
    }
    
    ESP_LOGD(TAG, "I2S stopped");
    return 0;
}

int32_t native_i2s_deinit(wasm_exec_env_t exec_env) {
    if (!i2s_initialized) {
        return 0;
    }
    
    // Stop playback
    native_i2s_stop_tone(exec_env);
    
    // Clean up timer
    if (i2s_timer_handle) {
        esp_timer_delete(i2s_timer_handle);
        i2s_timer_handle = NULL;
    }
    
    // Uninstall I2S driver
    i2s_driver_uninstall(I2S_NUM_0);
    
    // Free sine table
    if (sine_table) {
        mcp_free(sine_table);
        sine_table = NULL;
    }
    
    i2s_initialized = false;
    ESP_LOGI(TAG, "I2S deinitialized");
    
    return 0;
}

int32_t native_i2s_play_tone(wasm_exec_env_t exec_env, int32_t frequency_hz, int32_t duration_ms, int32_t volume_percent) {
    if (!i2s_initialized) {
        ESP_LOGE(TAG, "I2S not initialized");
        return -1;
    }
    
    // Validate parameters
    if (frequency_hz < 20 || frequency_hz > 20000) {
        ESP_LOGE(TAG, "Frequency out of range: %d Hz", frequency_hz);
        return -2;
    }
    
    if (volume_percent < 0 || volume_percent > 100) {
        ESP_LOGE(TAG, "Volume out of range: %d%%", volume_percent);
        return -3;
    }
    
    // Stop any current playback
    native_i2s_stop_tone(exec_env);
    
    // Set audio parameters
    current_frequency = (float)frequency_hz;
    i2s_volume = volume_percent;
    phase_accumulator = 0.0f;
    i2s_playing = true;
    
    // Create audio generation task
    BaseType_t task_ret = xTaskCreate(
        i2s_audio_task,
        "i2s_audio",
        4096,
        NULL,
        5,
        &i2s_task_handle
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2S audio task");
        i2s_playing = false;
        return -4;
    }
    
    // Set duration timer if specified
    if (duration_ms > 0) {
        esp_err_t ret = esp_timer_start_once(i2s_timer_handle, duration_ms * 1000);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start duration timer: %s", esp_err_to_name(ret));
        }
    }
    
    ESP_LOGI(TAG, "Playing I2S tone: %d Hz, %d ms, %d%% volume", frequency_hz, duration_ms, volume_percent);
    
    return 0;
}

int32_t native_i2s_stop_tone(wasm_exec_env_t exec_env) {
    if (!i2s_initialized) {
        return -1;
    }
    
    // Stop playback
    i2s_playing = false;
    
    // Stop timer if running
    if (i2s_timer_handle) {
        esp_timer_stop(i2s_timer_handle);
    }
    
    // Wait for task to finish
    if (i2s_task_handle) {
        // Give task time to exit gracefully
        vTaskDelay(pdMS_TO_TICKS(50));
        if (i2s_task_handle) {
            vTaskDelete(i2s_task_handle);
            i2s_task_handle = NULL;
        }
    }
    
    // Clear I2S DMA buffer
    i2s_zero_dma_buffer(I2S_NUM_0);
    
    ESP_LOGI(TAG, "I2S tone stopped");
    
    return 0;
}

int32_t native_i2s_set_volume(wasm_exec_env_t exec_env, int32_t volume_percent) {
    if (volume_percent < 0 || volume_percent > 100) {
        ESP_LOGE(TAG, "Volume out of range: %d%%", volume_percent);
        return -1;
    }
    
    i2s_volume = volume_percent;
    ESP_LOGI(TAG, "I2S volume set to %d%%", volume_percent);
    
    return 0;
}

int32_t native_i2s_is_playing(wasm_exec_env_t exec_env) {
    return i2s_playing ? 1 : 0;
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
    // {"strcat", mcp_native_strcat, "($$)i", NULL},
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