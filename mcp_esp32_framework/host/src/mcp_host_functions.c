#include "mcp_host_functions.h"
#include "mcp_config_loader.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "mcp_config.h"
#include "mcp_http_server.h"

static const char* TAG = "MCP_HOST";

// WASM memory alignment size
#define MALLOC_ALIGN_SIZE 8

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

void mcp_native_free(wasm_exec_env_t exec_env, uint32_t ptr) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    if (ptr == 0) {
        return; // Nothing to free
    }
    
    wasm_runtime_module_free(module_inst, ptr);
}

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

int32_t mcp_native_strncmp(wasm_exec_env_t exec_env, const char* s1, const char* s2, unsigned long n) {
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

static int32_t native_gpio_set_level(wasm_exec_env_t exec_env, int32_t pin, int32_t level)
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

static int32_t native_gpio_get_level(wasm_exec_env_t exec_env, int32_t pin)
{
    if (pin < 0 || pin >= 40) {
        return -1; // Invalid pin
    }
    
    return gpio_get_level((gpio_num_t)pin);
}

static int32_t native_gpio_set_direction(wasm_exec_env_t exec_env, int32_t pin, int32_t direction)
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

static int32_t native_gpio_is_pin_protected(wasm_exec_env_t exec_env, int32_t pin)
{
    for (int i = 0; i < g_mcp_config.gpio_protection.protected_pin_count; i++) {
        if (g_mcp_config.gpio_protection.protected_pins[i] == pin) {
            return 1; // Protected
        }
    }
    return 0; // Not protected
}

// ========== WAMR Memory Allocator Access Functions ==========

void mcp_get_wamr_allocators(void** malloc_func, void** realloc_func, void** free_func) {
    if (malloc_func) *malloc_func = (void*)wamr_malloc;
    if (realloc_func) *realloc_func = (void*)wamr_realloc;  
    if (free_func) *free_func = (void*)wamr_free;
}

// ========== SSE Event Functions (MCP Protocol) ==========

static int32_t native_sse_send_event(wasm_exec_env_t exec_env, const char* event_type, const char* data)
{
    // Using automatic string conversion ($$) - no validation needed
    ESP_LOGI(TAG, "WASM sending SSE event: %s = %s", event_type, data);
    // TODO: Implement send_sse_event function or move to application-specific code
    ESP_LOGW(TAG, "SSE event sending not implemented yet");
    return 0;
}

static int32_t native_sse_send_tool_result(wasm_exec_env_t exec_env, const char* tool_name, const char* result)
{
    // Using automatic string conversion ($$) - no validation needed
    
    // Create JSON-formatted tool result
    char event_data[512];
    snprintf(event_data, sizeof(event_data), "{\"tool\":\"%s\",\"result\":%s}", tool_name, result);
    // TODO: Implement send_sse_event function or move to application-specific code
    ESP_LOGI(TAG, "WASM tool result: %s", event_data);
    ESP_LOGW(TAG, "SSE tool result sending not implemented yet");
    return 0;
}

// ========== JSON-RPC Protocol Functions (MCP Framework) ==========

static int32_t native_json_parse_request(wasm_exec_env_t exec_env, const char* json_str, 
                                         char* method_out, char* params_out, int32_t max_len)
{
    ESP_LOGD(TAG, "JSON_PARSE: Called with json=%p, method=%p, params=%p, max_len=%d", json_str, method_out, params_out, max_len);
    
    // Since we're using automatic conversion ($**i), the pointers are already native pointers
    // No validation needed - just direct access
    
    ESP_LOGD(TAG, "JSON_PARSE: All validations passed, JSON content: '%.50s'", json_str);
    
    // Parse JSON-RPC method
    ESP_LOGD(TAG, "JSON_PARSE: About to call json_find_string");
    if (!mcp_json_find_string(json_str, "method", method_out, 64)) {  // method buffer is 64 bytes
        ESP_LOGE(TAG, "JSON_PARSE: json_find_string failed - method not found");
        return -2; // Method not found
    }
    
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

static int32_t native_json_create_response(wasm_exec_env_t exec_env, const char* result, char* response, int32_t max_len)
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

static int32_t native_json_create_error(wasm_exec_env_t exec_env, int32_t code, const char* message, char* response, int32_t max_len)
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

// ========== Host Function Registration ==========

NativeSymbol mcp_core_host_functions[] = {
    // Memory management
    {"malloc", mcp_native_malloc, "(i)i", NULL},
    {"free", mcp_native_free, "(i)", NULL},
    
    // String functions - using automatic string conversion ($)
    {"strlen", mcp_native_strlen, "($)i", NULL},
    {"wasm_strcmp", mcp_native_strcmp, "($$)i", NULL},
    {"strcpy", mcp_native_strcpy, "($$)i", NULL},
    {"strcat", mcp_native_strcat, "($$)i", NULL},
    {"wasm_strncpy", mcp_native_strncpy, "($*i)i", NULL},
    {"strstr", mcp_native_strstr, "($$)i", NULL},
    
    // Memory functions
    {"memcpy", mcp_native_memcpy, "(**i)i", NULL},
    {"memcmp", mcp_native_memcmp, "(**i)i", NULL},
    {"strncmp", mcp_native_strncmp, "($$i)i", NULL},
    
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
    
    // SSE Events (MCP Protocol)
    {"sse_send_event", native_sse_send_event, "($$)i", NULL},
    {"sse_send_tool_result", native_sse_send_tool_result, "($$)i", NULL},
    
    // JSON-RPC Protocol (MCP Framework)
    {"json_parse_request", native_json_parse_request, "($**i)i", NULL},
    {"json_create_response", native_json_create_response, "($*~)i", NULL},
    {"json_create_error", native_json_create_error, "(i$*~)i", NULL},
};

const int mcp_core_host_functions_count = sizeof(mcp_core_host_functions) / sizeof(NativeSymbol);