#include "mcp_wasm_runtime.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mcp_wasm_runtime";

// Initialize WASM runtime system
bool mcp_wasm_runtime_init(mcp_wasm_runtime_t *runtime) {
    if (!runtime) {
        ESP_LOGE(TAG, "Runtime structure is NULL");
        return false;
    }
    
    // Initialize structure
    memset(runtime, 0, sizeof(mcp_wasm_runtime_t));
    
    // Note: The actual wasm_runtime_init or wasm_runtime_full_init
    // is expected to be called by the application with proper
    // memory allocation configuration
    
    ESP_LOGI(TAG, "WASM runtime structure initialized");
    return true;
}

// Initialize WAMR with full configuration
bool mcp_wasm_runtime_full_init(RuntimeInitArgs *init_args) {
    if (!init_args) {
        ESP_LOGE(TAG, "Init args is NULL");
        return false;
    }
    
    if (!wasm_runtime_full_init(init_args)) {
        ESP_LOGE(TAG, "WASM runtime full init failed");
        return false;
    }
    
    ESP_LOGI(TAG, "WASM runtime fully initialized with custom configuration");
    return true;
}

// Load WASM module from file
bool mcp_wasm_load_module_from_file(mcp_wasm_runtime_t *runtime, 
                                    const char *filename) {
    if (!runtime || !filename) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }
    
    // Open file
    FILE *file = fopen(filename, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        snprintf(runtime->error_buf, sizeof(runtime->error_buf), 
                 "Failed to open file: %s", filename);
        return false;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    runtime->wasm_file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Allocate buffer
    runtime->wasm_file_buf = malloc(runtime->wasm_file_size);
    if (!runtime->wasm_file_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for WASM file");
        snprintf(runtime->error_buf, sizeof(runtime->error_buf), 
                 "Failed to allocate memory for WASM file");
        fclose(file);
        return false;
    }
    
    // Read file
    size_t read_size = fread(runtime->wasm_file_buf, 1, runtime->wasm_file_size, file);
    fclose(file);
    
    if (read_size != runtime->wasm_file_size) {
        ESP_LOGE(TAG, "Failed to read WASM file");
        snprintf(runtime->error_buf, sizeof(runtime->error_buf), 
                 "Failed to read WASM file");
        free(runtime->wasm_file_buf);
        runtime->wasm_file_buf = NULL;
        return false;
    }
    
    // Load WASM module
    runtime->module = wasm_runtime_load(runtime->wasm_file_buf, 
                                       runtime->wasm_file_size, 
                                       runtime->error_buf, 
                                       sizeof(runtime->error_buf));
    
    if (!runtime->module) {
        ESP_LOGE(TAG, "Failed to load WASM module: %s", runtime->error_buf);
        free(runtime->wasm_file_buf);
        runtime->wasm_file_buf = NULL;
        return false;
    }
    
    ESP_LOGI(TAG, "WASM module loaded successfully from %s", filename);
    return true;
}

// Register native host functions
bool mcp_wasm_register_natives(const char *module_name,
                              NativeSymbol *natives, 
                              uint32_t n_natives) {
    if (!module_name || !natives || n_natives == 0) {
        ESP_LOGE(TAG, "Invalid native registration parameters");
        return false;
    }
    
    if (!wasm_runtime_register_natives(module_name, natives, n_natives)) {
        ESP_LOGE(TAG, "Failed to register native functions");
        return false;
    }
    
    ESP_LOGI(TAG, "Registered %u native functions for module '%s'", 
             n_natives, module_name);
    return true;
}

// Create module instance and execution environment
bool mcp_wasm_instantiate_module(mcp_wasm_runtime_t *runtime,
                                uint32_t stack_size,
                                uint32_t heap_size) {
    if (!runtime || !runtime->module) {
        ESP_LOGE(TAG, "Invalid runtime or module not loaded");
        return false;
    }
    
    // Instantiate the module
    runtime->module_inst = wasm_runtime_instantiate(runtime->module, 
                                                   stack_size, 
                                                   heap_size,
                                                   runtime->error_buf, 
                                                   sizeof(runtime->error_buf));
    
    if (!runtime->module_inst) {
        ESP_LOGE(TAG, "Failed to instantiate WASM module: %s", runtime->error_buf);
        return false;
    }
    
    // Create execution environment
    runtime->exec_env = wasm_runtime_create_exec_env(runtime->module_inst, stack_size);
    if (!runtime->exec_env) {
        ESP_LOGE(TAG, "Failed to create execution environment");
        snprintf(runtime->error_buf, sizeof(runtime->error_buf), 
                 "Failed to create execution environment");
        wasm_runtime_deinstantiate(runtime->module_inst);
        runtime->module_inst = NULL;
        return false;
    }
    
    ESP_LOGI(TAG, "WASM module instantiated successfully (stack: %u, heap: %u)", 
             stack_size, heap_size);
    return true;
}

// Execute WASM function
bool mcp_wasm_call_function(mcp_wasm_runtime_t *runtime,
                           const char *func_name,
                           uint32_t argc, 
                           uint32_t argv[]) {
    if (!runtime || !runtime->module_inst || !runtime->exec_env || !func_name) {
        ESP_LOGE(TAG, "Invalid runtime state or parameters");
        return false;
    }
    
    // Get function
    wasm_function_inst_t func = wasm_runtime_lookup_function(runtime->module_inst, 
                                                             func_name);
    if (!func) {
        ESP_LOGE(TAG, "Function '%s' not found in WASM module", func_name);
        snprintf(runtime->error_buf, sizeof(runtime->error_buf), 
                 "Function '%s' not found", func_name);
        return false;
    }
    
    // Call function
    if (!wasm_runtime_call_wasm(runtime->exec_env, func, argc, argv)) {
        const char *exception = wasm_runtime_get_exception(runtime->module_inst);
        ESP_LOGE(TAG, "Failed to call WASM function '%s': %s", 
                 func_name, exception ? exception : "unknown error");
        snprintf(runtime->error_buf, sizeof(runtime->error_buf), 
                 "Function call failed: %s", exception ? exception : "unknown error");
        return false;
    }
    
    ESP_LOGD(TAG, "Successfully called WASM function '%s'", func_name);
    return true;
}

// Get function from module
wasm_function_inst_t mcp_wasm_get_function(mcp_wasm_runtime_t *runtime,
                                           const char *func_name) {
    if (!runtime || !runtime->module_inst || !func_name) {
        return NULL;
    }
    
    return wasm_runtime_lookup_function(runtime->module_inst, func_name);
}

// Cleanup and destroy runtime
void mcp_wasm_runtime_cleanup(mcp_wasm_runtime_t *runtime) {
    if (!runtime) {
        return;
    }
    
    // Destroy execution environment
    if (runtime->exec_env) {
        wasm_runtime_destroy_exec_env(runtime->exec_env);
        runtime->exec_env = NULL;
    }
    
    // Deinstantiate module
    if (runtime->module_inst) {
        wasm_runtime_deinstantiate(runtime->module_inst);
        runtime->module_inst = NULL;
    }
    
    // Unload module
    if (runtime->module) {
        wasm_runtime_unload(runtime->module);
        runtime->module = NULL;
    }
    
    // Free WASM file buffer
    if (runtime->wasm_file_buf) {
        free(runtime->wasm_file_buf);
        runtime->wasm_file_buf = NULL;
    }
    
    // Destroy WASM runtime
    wasm_runtime_destroy();
    
    ESP_LOGI(TAG, "WASM runtime cleaned up");
}

// Get last error message
const char* mcp_wasm_get_last_error(mcp_wasm_runtime_t *runtime) {
    if (!runtime) {
        return "Runtime structure is NULL";
    }
    return runtime->error_buf;
}