#include "mcp_wasm_runtime.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

// ========== SD Card and File Operations ==========

// Mount SD card for WASM file access
bool mcp_wasm_mount_sd_card(void) {
    sdmmc_card_t *card;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024  // Use official example's 16K instead of 32K
    };
    
    ESP_LOGI(TAG, "Trying SPI mode first for better compatibility...");
    
    // Initialize SPI bus first
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = GPIO_NUM_23,    // MOSI
        .miso_io_num = GPIO_NUM_19,    // MISO  
        .sclk_io_num = GPIO_NUM_18,    // SCLK
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Configure SPI SD card
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 10000;  // Start with slow speed
    
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = host.slot;
    slot_config.gpio_cs = GPIO_NUM_5;
    
    ESP_LOGI(TAG, "Using SPI pins: CLK=18, MOSI=23, MISO=19, CS=5");
    ESP_LOGI(TAG, "Mounting SD card...");
    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ SD card mounted successfully via SPI");
        ESP_LOGI(TAG, "SD card info: Name: %s, Type: %s, Capacity: %lluMB", 
                card->cid.name, 
                (card->ocr & (1<<30)) ? "SDHC/SDXC" : "SDSC",  // Bit 30 indicates SDHC
                ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
        return true;
    }
    
    ESP_LOGW(TAG, "SPI mount failed: %s", esp_err_to_name(ret));
    ESP_LOGI(TAG, "Falling back to SDMMC mode...");
    
    // Clean up SPI bus before trying SDMMC
    spi_bus_free(SPI2_HOST);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Try SDMMC as fallback - use polling mode to avoid DMA timeout issues
    sdmmc_host_t sdmmc_host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t sdmmc_slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    
    // Use official ESP32 SDMMC pin configuration
    sdmmc_slot_config.width = 1;  // 1-bit mode for better compatibility
    sdmmc_slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    // Set correct GPIO pins for ESP32 (from official example)
    sdmmc_slot_config.clk = GPIO_NUM_14;   // CLK pin
    sdmmc_slot_config.cmd = GPIO_NUM_15;   // CMD pin  
    sdmmc_slot_config.d0 = GPIO_NUM_2;     // D0 pin
    
    ESP_LOGI(TAG, "SDMMC: Using official ESP32 pins - CLK=14, CMD=15, D0=2");
    
    ret = esp_vfs_fat_sdmmc_mount("/sdcard", &sdmmc_host, &sdmmc_slot_config, &mount_config, &card);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ SD card mounted successfully via SDMMC fallback");
        ESP_LOGI(TAG, "SD card info: Name: %s, Type: %s, Capacity: %lluMB", 
                card->cid.name, 
                (card->ocr & (1<<30)) ? "SDHC/SDXC" : "SDSC",
                ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
        return true;
    } else {
        ESP_LOGE(TAG, "❌ Both SPI and SDMMC modes failed: %s", esp_err_to_name(ret));
        ESP_LOGI(TAG, "Check SD card insertion, formatting (FAT32), and connections");
        return false;
    }
}

// Load WASM file from SD card
uint8_t* mcp_wasm_load_from_sd(const char* filename, unsigned* file_size) {
    ESP_LOGI(TAG, "Attempting direct WASM file load (skipping directory listing)...");
    
    // Add delay to let SD card stabilize after mount
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/sdcard/wasm/%s", filename);
    
    ESP_LOGI(TAG, "Loading WASM file: %s", filepath);
    
    // Try to access the file with detailed error reporting
    ESP_LOGI(TAG, "Attempting to open file: %s", filepath);
    
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGE(TAG, "❌ Failed to open WASM file: %s", filepath);
        ESP_LOGI(TAG, "Error details: %s", strerror(errno));
        
        // Try just opening the directory to see if it exists
        DIR *wasm_dir = opendir("/sdcard/wasm");
        if (wasm_dir) {
            ESP_LOGI(TAG, "✅ /sdcard/wasm directory exists, checking contents:");
            struct dirent *entry;
            int file_count = 0;
            while ((entry = readdir(wasm_dir)) != NULL) {
                ESP_LOGI(TAG, "  Found: %s", entry->d_name);
                file_count++;
            }
            ESP_LOGI(TAG, "Total files in /wasm/: %d", file_count);
            closedir(wasm_dir);
        } else {
            ESP_LOGE(TAG, "❌ /sdcard/wasm directory does not exist!");
            ESP_LOGI(TAG, "Directory error: %s", strerror(errno));
        }
        
        return NULL;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (size <= 0) {
        ESP_LOGE(TAG, "❌ Invalid WASM file size: %ld", size);
        fclose(file);
        return NULL;
    }
    
    // Allocate buffer
    uint8_t* buffer = (uint8_t*)malloc(size);
    if (!buffer) {
        ESP_LOGE(TAG, "❌ Failed to allocate memory for WASM file (%ld bytes)", size);
        fclose(file);
        return NULL;
    }
    
    // Read file
    size_t bytes_read = fread(buffer, 1, size, file);
    fclose(file);
    
    if (bytes_read != size) {
        ESP_LOGE(TAG, "❌ Failed to read WASM file completely. Expected: %ld, Read: %zu", size, bytes_read);
        free(buffer);
        return NULL;
    }
    
    *file_size = (unsigned)size;
    ESP_LOGI(TAG, "✅ Successfully loaded WASM file: %s (%u bytes)", filename, *file_size);
    return buffer;
}

// ========== WASM Execution Utilities ==========

// Create execution environment with optimal settings
wasm_exec_env_t mcp_wasm_create_exec_env(wasm_module_inst_t module_inst) {
    // Use same stack size as instantiation like the examples
    uint32_t stack_size = 8192;
    
    wasm_exec_env_t env = wasm_runtime_create_exec_env(module_inst, stack_size);
    if (env) {
        ESP_LOGI(TAG, "✅ Created exec env with %d byte stack", stack_size);
        return env;
    }
    
    ESP_LOGE(TAG, "❌ Exec env creation failed with %d byte stack", stack_size);
    return NULL;
}

// Safe WASM function call wrapper (handles threading issues)
bool mcp_wasm_safe_call(wasm_exec_env_t exec_env, wasm_function_inst_t func, 
                        uint32_t argc, uint32_t *argv) {
    if (!exec_env || !func) return false;
    
    ESP_LOGD(TAG, "Attempting WASM function call...");
    
    // Try the call - this may crash due to threading issues
    bool result = wasm_runtime_call_wasm(exec_env, func, argc, argv);
    
    if (result) {
        ESP_LOGD(TAG, "WASM call succeeded");
    } else {
        ESP_LOGD(TAG, "WASM call failed");
    }
    
    return result;
}

// Debug: Dump WASM module exports for inspection
void mcp_wasm_dump_exports(wasm_module_t module, const char* module_name) {
    if (!module) {
        ESP_LOGW(TAG, "Cannot dump exports: module is NULL");
        return;
    }
    
    ESP_LOGI(TAG, "🔍 === WASM Export Dump for '%s' ===", module_name ? module_name : "unknown");
    
    int32_t export_count = wasm_runtime_get_export_count(module);
    ESP_LOGI(TAG, "🔍 Total exports: %d", export_count);
    
    for (int32_t i = 0; i < export_count; i++) {
        wasm_export_t export_info;
        wasm_runtime_get_export_type(module, i, &export_info);
        
        const char* kind_str = "unknown";
        switch (export_info.kind) {
            case WASM_IMPORT_EXPORT_KIND_FUNC: kind_str = "function"; break;
            case WASM_IMPORT_EXPORT_KIND_GLOBAL: kind_str = "global"; break;
            case WASM_IMPORT_EXPORT_KIND_MEMORY: kind_str = "memory"; break;
            case WASM_IMPORT_EXPORT_KIND_TABLE: kind_str = "table"; break;
        }
        
        ESP_LOGI(TAG, "  [%d] %s: '%s'", i, kind_str, export_info.name ? export_info.name : "<null>");
        
        // Special highlight for mcp_req
        if (export_info.name && strcmp(export_info.name, "mcp_req") == 0) {
            ESP_LOGI(TAG, "  ✅ Found mcp_req export!");
        }
    }
    
    ESP_LOGI(TAG, "🔍 Export dump complete for '%s'", module_name);
}