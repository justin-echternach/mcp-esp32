#include "mcp_host_functions.h"
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
#include "mcp_config.h"
#include "mcp_http_server.h"

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

static int32_t native_sse_send_tool_result(wasm_exec_env_t exec_env, const char* tool_name, const char* result)
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

// ========== Request Context Management ==========

void mcp_host_set_request_context(void* request) {
    tls_current_request = (mcp_request_t*)request;
}

void mcp_host_clear_request_context(void) {
    tls_current_request = NULL;
}

// ========== HTTP Header Management Functions ==========

// Helper function for case-insensitive string comparison
static int strcasecmp_custom(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = tolower((unsigned char)*s1);
        char c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

// Helper function to find header in string buffer (case-insensitive)
static const char* find_header_value(const char* headers, const char* name) {
    if (!headers || !name) return NULL;
    
    size_t name_len = strlen(name);
    const char* pos = headers;
    
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
            
            // Allocate buffer for value (this will be managed by WAMR)
            static char value_buffer[512];
            size_t value_len = end - value;
            if (value_len >= sizeof(value_buffer)) value_len = sizeof(value_buffer) - 1;
            
            strncpy(value_buffer, value, value_len);
            value_buffer[value_len] = '\0';
            
            return value_buffer;
        }
        
        // Move to next line
        const char* next_line = strstr(pos, "\r\n");
        if (!next_line) break;
        pos = next_line + 2;
    }
    
    return NULL;
}

// Helper function to ensure response headers buffer has enough capacity
static int ensure_response_headers_capacity(mcp_request_t* request, size_t needed) {
    if (!request->response_headers) {
        // Initial allocation
        request->response_headers_capacity = needed > 1024 ? needed : 1024;
        request->response_headers = malloc(request->response_headers_capacity);
        if (!request->response_headers) return -1;
        request->response_headers[0] = '\0';
        request->response_headers_length = 0;
    } else if (request->response_headers_length + needed >= request->response_headers_capacity) {
        // Realloc to larger size
        size_t new_capacity = (request->response_headers_capacity + needed) * 2;
        char* new_buffer = realloc(request->response_headers, new_capacity);
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

const char* mcp_native_get_request_header(wasm_exec_env_t exec_env, const char* name) {
    if (!tls_current_request) {
        ESP_LOGW(TAG, "get_request_header: No current request context");
        return NULL;
    }
    
    if (!name) {
        ESP_LOGE(TAG, "get_request_header: Invalid header name");
        return NULL;
    }
    
    const char* value = find_header_value(tls_current_request->headers, name);
    ESP_LOGD(TAG, "get_request_header: %s = %s", name, value ? value : "NULL");
    return value;
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

const char* mcp_native_list_request_headers(wasm_exec_env_t exec_env) {
    if (!tls_current_request || !tls_current_request->headers) {
        return "";
    }
    
    static char header_list[1024];
    header_list[0] = '\0';
    
    const char* pos = tls_current_request->headers;
    size_t list_len = 0;
    
    while (*pos && list_len < sizeof(header_list) - 50) {
        // Find header name (before colon)
        const char* colon = strchr(pos, ':');
        if (!colon) break;
        
        // Add comma if not first header
        if (list_len > 0) {
            strcat(header_list, ",");
            list_len++;
        }
        
        // Add header name
        size_t name_len = colon - pos;
        if (list_len + name_len < sizeof(header_list) - 1) {
            strncat(header_list, pos, name_len);
            list_len += name_len;
        }
        
        // Move to next line
        const char* next_line = strstr(pos, "\r\n");
        if (!next_line) break;
        pos = next_line + 2;
    }
    
    ESP_LOGD(TAG, "list_request_headers: %s", header_list);
    return header_list;
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
    
    // HTTP Header Management
    // DEBUGGING: Commenting out to test if these registrations cause WASM export corruption
    // {"set_response_header", mcp_native_set_response_header, "($$)i", NULL},
    // {"get_request_header", mcp_native_get_request_header, "($)$", NULL},
    // {"remove_response_header", mcp_native_remove_response_header, "($)i", NULL},
    // {"list_request_headers", mcp_native_list_request_headers, "()$", NULL},
};

const int mcp_core_host_functions_count = sizeof(mcp_core_host_functions) / sizeof(NativeSymbol);