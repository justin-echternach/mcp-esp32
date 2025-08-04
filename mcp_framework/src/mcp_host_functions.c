#include "mcp_host_functions.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "MCP_HOST";

// ========== Memory Management Functions ==========

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
    
    // Logging functions
    {"log_info", mcp_native_log_info, "($$)i", NULL},
    {"log_error", mcp_native_log_error, "($$)i", NULL},
    
    // System information
    {"get_free_heap", mcp_native_get_free_heap, "()i", NULL},
    {"get_uptime_ms", mcp_native_get_uptime_ms, "()i", NULL},
    {"get_tick_count", mcp_native_get_tick_count, "()i", NULL},
};

const int mcp_core_host_functions_count = sizeof(mcp_core_host_functions) / sizeof(NativeSymbol);