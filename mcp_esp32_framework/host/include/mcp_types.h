#ifndef MCP_TYPES_H
#define MCP_TYPES_H

#include <stdbool.h>
#include <pthread.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== WASM Module Types ==========
// Types are defined in mcp_pthread_handler.h - include that instead
// This file will contain only new framework-specific types

// ========== Error Codes ==========

/**
 * @brief MCP-specific error codes
 */
typedef enum {
    MCP_OK = 0,                                 // Success
    MCP_ERR_INVALID_PARAM = -1,                // Invalid parameter
    MCP_ERR_NO_MEMORY = -2,                    // Out of memory
    MCP_ERR_MODULE_NOT_FOUND = -3,             // WASM module not found
    MCP_ERR_MODULE_LOAD_FAILED = -4,           // WASM module load failed
    MCP_ERR_FUNCTION_NOT_FOUND = -5,           // WASM function not found
    MCP_ERR_EXECUTION_FAILED = -6,             // WASM execution failed
    MCP_ERR_TIMEOUT = -7,                      // Request timeout
    MCP_ERR_JSON_PARSE = -8,                   // JSON parsing error
    MCP_ERR_GPIO_PROTECTED = -9,               // GPIO pin is protected
} mcp_error_t;

#ifdef __cplusplus
}
#endif

#endif // MCP_TYPES_H