#ifndef MCP_HOST_FUNCTIONS_H
#define MCP_HOST_FUNCTIONS_H

#include <stdint.h>
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== Core Host Functions for WASM Modules ==========

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