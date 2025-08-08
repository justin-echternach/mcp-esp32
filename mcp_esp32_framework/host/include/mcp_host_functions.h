#ifndef MCP_HOST_FUNCTIONS_H
#define MCP_HOST_FUNCTIONS_H

#include <stdint.h>
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== Core Host Functions for WASM Modules ==========

int32_t mcp_native_atoi(wasm_exec_env_t exec_env, const char* str);
int32_t mcp_native_memcmp(wasm_exec_env_t exec_env, const void* s1, const void* s2, int32_t n);
int32_t mcp_native_strncmp(wasm_exec_env_t exec_env, const char* s1, const char* s2, int32_t n);

// ========== Memory Management Functions ==========

/**
 * @brief Allocate memory in WASM linear memory space
 * 
 * @param exec_env WASM execution environment
 * @param size Size in bytes to allocate
 * @return WASM address offset (0 if allocation failed)
 */
uint32_t mcp_native_malloc(wasm_exec_env_t exec_env, uint32_t size);

/**
 * @brief Free memory in WASM linear memory space
 * 
 * @param exec_env WASM execution environment  
 * @param ptr WASM address offset to free
 */
void mcp_native_free(wasm_exec_env_t exec_env, uint32_t ptr);

// ========== String Functions ==========

/**
 * @brief Get length of string
 * 
 * @param exec_env WASM execution environment
 * @param s String to measure (WASM validates address)
 * @return Length of string in bytes
 */
int32_t mcp_native_strlen(wasm_exec_env_t exec_env, const char* s);

/**
 * @brief Compare two strings
 * 
 * @param exec_env WASM execution environment
 * @param s1 First string (WASM validates address)
 * @param s2 Second string (WASM validates address)
 * @return <0 if s1<s2, 0 if equal, >0 if s1>s2
 */
int32_t mcp_native_strcmp(wasm_exec_env_t exec_env, const char* s1, const char* s2);

/**
 * @brief Copy string
 * 
 * @param exec_env WASM execution environment
 * @param dest Destination buffer (WASM validates address)
 * @param src Source string (WASM validates address)
 * @return WASM address of destination string
 */
int32_t mcp_native_strcpy(wasm_exec_env_t exec_env, char* dest, const char* src);

/**
 * @brief Concatenate strings
 * 
 * @param exec_env WASM execution environment
 * @param dest Destination buffer (WASM validates address)
 * @param src Source string to append (WASM validates address)
 * @return WASM address of destination string
 */
int32_t mcp_native_strcat(wasm_exec_env_t exec_env, char* dest, const char* src);

/**
 * @brief Copy string with length limit
 * 
 * @param exec_env WASM execution environment
 * @param dest Destination buffer (WASM validates address)
 * @param src Source string (WASM validates address)
 * @param n Maximum characters to copy
 * @return WASM address of destination string
 */
int32_t mcp_native_strncpy(wasm_exec_env_t exec_env, char* dest, const char* src, int32_t n);

/**
 * @brief Find substring in string
 * 
 * @param exec_env WASM execution environment
 * @param haystack String to search in (WASM validates address)
 * @param needle Substring to find (WASM validates address)
 * @return WASM address of first occurrence (0 if not found)
 */
int32_t mcp_native_strstr(wasm_exec_env_t exec_env, const char* haystack, const char* needle);

// ========== Memory Functions ==========

/**
 * @brief Copy memory block
 * 
 * @param exec_env WASM execution environment
 * @param dest Destination buffer (WASM validates address)
 * @param src Source buffer (WASM validates address)
 * @param n Number of bytes to copy
 * @return WASM address of destination buffer
 */
int32_t mcp_native_memcpy(wasm_exec_env_t exec_env, void* dest, const void* src, int32_t n);

// ========== Logging Functions ==========

/**
 * @brief Log info message
 * 
 * @param exec_env WASM execution environment
 * @param tag Log tag (WASM validates address)
 * @param message Log message (WASM validates address)
 * @return 0 on success, -1 on error
 */
int32_t mcp_native_log_info(wasm_exec_env_t exec_env, const char* tag, const char* message);

/**
 * @brief Log error message
 * 
 * @param exec_env WASM execution environment
 * @param tag Log tag (WASM validates address)
 * @param message Log message (WASM validates address)
 * @return 0 on success, -1 on error
 */
int32_t mcp_native_log_error(wasm_exec_env_t exec_env, const char* tag, const char* message);

// ========== System Information Functions ==========

/**
 * @brief Get free heap size
 * 
 * @param exec_env WASM execution environment
 * @return Free heap size in bytes
 */
int32_t mcp_native_get_free_heap(wasm_exec_env_t exec_env);

/**
 * @brief Get system uptime
 * 
 * @param exec_env WASM execution environment
 * @return Uptime in milliseconds
 */
int32_t mcp_native_get_uptime_ms(wasm_exec_env_t exec_env);

/**
 * @brief Get system tick count
 * 
 * @param exec_env WASM execution environment
 * @return Current tick count
 */
int32_t mcp_native_get_tick_count(wasm_exec_env_t exec_env);

// ========== GPIO Control Functions ==========
/**
 * @brief Set GPIO pin level
 * 
 * @param exec_env WASM execution environment
 * @param pin GPIO pin number (0-39)
 * @param level Level to set (0 or 1)
 * @return 0 on success, -1 on error
 */
int32_t native_gpio_set_level(wasm_exec_env_t exec_env, int32_t pin, int32_t level);
int32_t native_gpio_get_level(wasm_exec_env_t exec_env, int32_t pin);
int32_t native_gpio_set_direction(wasm_exec_env_t exec_env, int32_t pin, int32_t direction);
int32_t native_gpio_is_pin_protected(wasm_exec_env_t exec_env, int32_t pin);

// ========== PWM Control Functions (LEDC-based) ==========

/**
 * @brief Initialize PWM output on specified pin with frequency
 * 
 * @param exec_env WASM execution environment
 * @param pin GPIO pin number (0-39, must not be protected)
 * @param frequency PWM frequency in Hz (1-40000)
 * @return 0 on success, negative on error
 */
int32_t native_pwm_init(wasm_exec_env_t exec_env, int32_t pin, int32_t frequency);

/**
 * @brief Set PWM duty cycle percentage
 * 
 * @param exec_env WASM execution environment
 * @param pin GPIO pin number (must be previously initialized)
 * @param duty_percent Duty cycle percentage (0-100)
 * @return 0 on success, negative on error
 */
int32_t native_pwm_set_duty(wasm_exec_env_t exec_env, int32_t pin, int32_t duty_percent);

/**
 * @brief Stop PWM output on specified pin
 * 
 * @param exec_env WASM execution environment
 * @param pin GPIO pin number
 * @return 0 on success, negative on error
 */
int32_t native_pwm_stop(wasm_exec_env_t exec_env, int32_t pin);

/**
 * @brief Update PWM frequency without reinitializing
 * 
 * @param exec_env WASM execution environment
 * @param pin GPIO pin number (must be previously initialized)
 * @param frequency New PWM frequency in Hz (1-40000)
 * @return 0 on success, negative on error
 */
int32_t native_pwm_set_frequency(wasm_exec_env_t exec_env, int32_t pin, int32_t frequency);

// ========== WAMR Runtime Memory Allocators (ESP32-specific) ==========

/**
 * @brief Get ESP32-specific WAMR memory allocator functions
 * 
 * These functions use ESP32 heap capabilities with SPIRAM support
 * for optimal memory allocation in WAMR runtime
 * 
 * @param malloc_func Pointer to receive malloc function
 * @param realloc_func Pointer to receive realloc function  
 * @param free_func Pointer to receive free function
 */
void mcp_get_wamr_allocators(void** malloc_func, void** realloc_func, void** free_func);

// ========== HTTP Header Management Functions ==========

/**
 * @brief Set HTTP response header from WASM context
 * 
 * @param exec_env WASM execution environment
 * @param name Header name
 * @param value Header value  
 * @return int32_t 0 on success, negative on error
 */
int32_t mcp_native_set_response_header(wasm_exec_env_t exec_env, const char* name, const char* value);

/**
 * @brief Get HTTP request header from WASM context (buffer pattern)
 * 
 * @param exec_env WASM execution environment
 * @param name Header name (case-insensitive)
 * @param buffer Buffer to write header value
 * @param buffer_size Size of buffer
 * @return int32_t Length of header value or -1 if not found
 */
int32_t mcp_native_get_request_header(wasm_exec_env_t exec_env, const char* name, char* buffer, int32_t buffer_size);

/**
 * @brief Remove HTTP response header from WASM context
 * 
 * @param exec_env WASM execution environment
 * @param name Header name
 * @return int32_t 0 on success, -1 if header not found
 */
int32_t mcp_native_remove_response_header(wasm_exec_env_t exec_env, const char* name);

/**
 * @brief List all available request headers (buffer pattern)
 * 
 * @param exec_env WASM execution environment
 * @param buffer Buffer to write header names (comma-separated)
 * @param buffer_size Size of buffer
 * @return int32_t Length of header list or -1 on error
 */
int32_t mcp_native_list_request_headers(wasm_exec_env_t exec_env, char* buffer, int32_t buffer_size);


int32_t native_sse_send_event(wasm_exec_env_t exec_env, const char* event_type, const char* data);
int32_t native_sse_send_tool_result(wasm_exec_env_t exec_env, const char* tool_name, const char* result);
int32_t native_json_parse_request(wasm_exec_env_t exec_env, const char* json_str, char* method_out, char* params_out, int32_t max_len);
int32_t native_json_create_response(wasm_exec_env_t exec_env, const char* result, char* response, int32_t max_len);
int32_t native_json_create_error(wasm_exec_env_t exec_env, int32_t code, const char* message, char* response, int32_t max_len);

// ========== Request Context Management (Internal) ==========

/**
 * @brief Set current request context for header functions (internal use)
 * 
 * @param request Current MCP request structure
 */
void mcp_host_set_request_context(void* request);

/**
 * @brief Clear current request context (internal use)
 */
void mcp_host_clear_request_context(void);

// ========== Host Function Registration ==========

/**
 * @brief Native symbol definitions for core host functions
 * 
 * Use this array to register core host functions with WAMR runtime
 */
extern NativeSymbol mcp_core_host_functions[];

/**
 * @brief Number of core host functions
 */
extern const int mcp_core_host_functions_count;

#ifdef __cplusplus
}
#endif

#endif // MCP_HOST_FUNCTIONS_H