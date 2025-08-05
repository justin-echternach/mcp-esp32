#ifndef MCP_WASM_RUNTIME_H
#define MCP_WASM_RUNTIME_H

#include "wasm_export.h"
#include "mcp_pthread_pool.h"  // For mcp_wasm_module_t definition
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WASM runtime state structure
typedef struct {
    wasm_module_t module;
    wasm_module_inst_t module_inst;
    wasm_exec_env_t exec_env;
    uint8_t *wasm_file_buf;
    uint32_t wasm_file_size;
    char error_buf[256];
} mcp_wasm_runtime_t;

// Initialize WASM runtime system (structure only)
bool mcp_wasm_runtime_init(mcp_wasm_runtime_t *runtime);

// Initialize WAMR with full configuration
bool mcp_wasm_runtime_full_init(RuntimeInitArgs *init_args);

// Load WASM module from file
bool mcp_wasm_load_module_from_file(mcp_wasm_runtime_t *runtime, 
                                    const char *filename);

// Register native host functions
bool mcp_wasm_register_natives(const char *module_name,
                              NativeSymbol *natives, 
                              uint32_t n_natives);

// Create module instance and execution environment
bool mcp_wasm_instantiate_module(mcp_wasm_runtime_t *runtime,
                                uint32_t stack_size,
                                uint32_t heap_size);

// Execute WASM function
bool mcp_wasm_call_function(mcp_wasm_runtime_t *runtime,
                           const char *func_name,
                           uint32_t argc, 
                           uint32_t argv[]);

// Get function from module
wasm_function_inst_t mcp_wasm_get_function(mcp_wasm_runtime_t *runtime,
                                           const char *func_name);

// Cleanup and destroy runtime
void mcp_wasm_runtime_cleanup(mcp_wasm_runtime_t *runtime);

// Get last error message
const char* mcp_wasm_get_last_error(mcp_wasm_runtime_t *runtime);

// ========== SD Card and File Operations ==========

// Mount SD card for WASM file access
bool mcp_wasm_mount_sd_card(void);

// Load WASM file from SD card
uint8_t* mcp_wasm_load_from_sd(const char* filename, unsigned* file_size);

// ========== WASM Execution Utilities ==========

// Create execution environment with optimal settings
wasm_exec_env_t mcp_wasm_create_exec_env(wasm_module_inst_t module_inst);

// Safe WASM function call wrapper (handles threading issues)
bool mcp_wasm_safe_call(wasm_exec_env_t exec_env, wasm_function_inst_t func, 
                        uint32_t argc, uint32_t *argv);

// Debug: Dump WASM module exports for inspection
void mcp_wasm_dump_exports(wasm_module_t module, const char* module_name);

// ========== MCP Module Registry ==========

// Initialize MCP module registry
bool mcp_wasm_registry_init(void);

// Load MCP WASM module for a specific route
mcp_wasm_module_t* mcp_wasm_load_module_for_route(const char* route, const char* wasm_filename);

// Find WASM filename for a given route
const char* mcp_wasm_find_file_for_route(const char* route);

// Get module registry statistics
void mcp_wasm_registry_stats(int* loaded_count, int* max_count);

// Cleanup MCP module registry
void mcp_wasm_registry_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // MCP_WASM_RUNTIME_H