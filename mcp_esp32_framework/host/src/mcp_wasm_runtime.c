#include "mcp_wasm_runtime.h"
#include "mcp_memory.h"
#include "mcp_config.h"
#include "mcp_host_functions.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mcp_wasm_runtime";

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
        static const int sd_spi_pins[] = {5, 18, 19, 23};  // CS, SCLK, MISO, MOSI
        mcp_host_reserve_pins(sd_spi_pins, 4, "SD card (SPI)");
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
        static const int sd_mmc_pins[] = {14, 15, 2};  // CLK, CMD, D0
        mcp_host_reserve_pins(sd_mmc_pins, 3, "SD card (SDMMC)");
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

// ========== MCP Module Registry Implementation ==========

// External reference to global MCP config (defined in main.c)
extern mcp_config_t g_mcp_config;

// MCP module registry globals
//static mcp_wasm_module_t g_mcp_modules[8]; // Max 8 MCP modules
static mcp_wasm_module_t *g_mcp_modules = NULL;
static int g_mcp_module_count = 0;
static pthread_mutex_t g_mcp_modules_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_registry_initialized = false;

bool mcp_wasm_registry_init(void) {
    if (g_registry_initialized) {
        ESP_LOGW(TAG, "MCP module registry already initialized");
        return true;
    }
    
    pthread_mutex_lock(&g_mcp_modules_mutex);
    
    // Initialize all modules to empty state
    g_mcp_modules = mcp_malloc_module_registry(8, sizeof(mcp_wasm_module_t));
    g_mcp_module_count = 0;
    g_registry_initialized = true;
    
    pthread_mutex_unlock(&g_mcp_modules_mutex);
    
    ESP_LOGI(TAG, "MCP module registry initialized successfully");
    return true;
}

mcp_wasm_module_t* mcp_wasm_load_module_for_route(const char* route, const char* wasm_filename) {
    if (!g_registry_initialized) {
        ESP_LOGE(TAG, "MCP module registry not initialized");
        return NULL;
    }
    
    pthread_mutex_lock(&g_mcp_modules_mutex);
    
    // Check if already loaded
    for (int i = 0; i < g_mcp_module_count; i++) {
        if (strcmp(g_mcp_modules[i].route, route) == 0 && g_mcp_modules[i].loaded) {
            pthread_mutex_unlock(&g_mcp_modules_mutex);
            return &g_mcp_modules[i];
        }
    }
    
    // Find empty slot
    if (g_mcp_module_count >= 8) {
        ESP_LOGE(TAG, "Maximum number of MCP modules reached");
        pthread_mutex_unlock(&g_mcp_modules_mutex);
        return NULL;
    }
    
    mcp_wasm_module_t* module = &g_mcp_modules[g_mcp_module_count];
    strncpy(module->route, route, sizeof(module->route) - 1);
    strncpy(module->wasm_file, wasm_filename, sizeof(module->wasm_file) - 1);
    module->loaded = false;
    module->watchdog = NULL;
    pthread_mutex_init(&module->module_mutex, NULL);
    
    // Load WASM file
    char wasm_path[128];
    snprintf(wasm_path, sizeof(wasm_path), "/sdcard/wasm/%s", wasm_filename);
    
    FILE *file = fopen(wasm_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open WASM file: %s", wasm_path);
        pthread_mutex_unlock(&g_mcp_modules_mutex);
        return NULL;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Memory debugging before allocation
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#ifdef CONFIG_SPIRAM
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Memory before WASM allocation: DRAM free=%zu, largest=%zu | PSRAM free=%zu, largest=%zu | need=%ld bytes", 
             free_heap, largest_block, psram_free, psram_largest, file_size);
#else
    ESP_LOGI(TAG, "Memory before WASM allocation: free=%zu bytes, largest_block=%zu bytes, need=%ld bytes", 
             free_heap, largest_block, file_size);
#endif
    
    // Allocate buffer - use smaller heap size to save memory
    //uint8_t *wasm_file_buf = heap_caps_malloc(file_size, MALLOC_CAP_8BIT);
    //uint8_t *wasm_file_buf = heap_caps_aligned_alloc(4, file_size, MALLOC_CAP_SPIRAM); // this meets the 4 byte alignment requirement for AOT and allows us to use psram
    uint8_t *wasm_file_buf = mcp_malloc_wasm_buffer(file_size);
    module->wasm_file_buf = wasm_file_buf; // this is crucial ... WAMR uses this buffer and it needs to stay around while the module is loaded
    module->wasm_file_size = file_size;

    ESP_LOGI(TAG, "Allocated %ld bytes for WASM file buffer at %p", file_size, wasm_file_buf);

    
    if (!wasm_file_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for WASM file (%ld bytes)", file_size);
#ifdef CONFIG_SPIRAM
        ESP_LOGE(TAG, "Memory after failed allocation: DRAM free=%zu, largest=%zu | PSRAM free=%zu, largest=%zu", 
                 heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#else
        ESP_LOGE(TAG, "Memory after failed allocation: free=%zu bytes, largest_block=%zu bytes", 
                 heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
        fclose(file);
        pthread_mutex_unlock(&g_mcp_modules_mutex);
        return NULL;
    }
    
    // Read file
    size_t bytes_read = fread(wasm_file_buf, 1, file_size, file);
    fclose(file);
    
    if (bytes_read != file_size) {
        ESP_LOGE(TAG, "Failed to read WASM file completely");
        mcp_free(wasm_file_buf);
        pthread_mutex_unlock(&g_mcp_modules_mutex);
        return NULL;
    }
    
    // DEBUGGING: Check WAMR compilation mode
    ESP_LOGI(TAG, "🔍 WAMR Compilation Mode Check:");
#ifdef WASM_ENABLE_INTERP
    ESP_LOGI(TAG, "  ✅ WASM_ENABLE_INTERP defined - Interpreter mode enabled");
#else
    ESP_LOGI(TAG, "  ❌ WASM_ENABLE_INTERP not defined");
#endif
#ifdef WASM_ENABLE_FAST_INTERP
    ESP_LOGI(TAG, "  ✅ WASM_ENABLE_FAST_INTERP defined - Fast interpreter enabled");
#else
    ESP_LOGI(TAG, "  ❌ WASM_ENABLE_FAST_INTERP not defined");
#endif
#ifdef WASM_ENABLE_AOT
    ESP_LOGI(TAG, "  ✅ WASM_ENABLE_AOT defined - AOT mode enabled");
#else
    ESP_LOGI(TAG, "  ❌ WASM_ENABLE_AOT not defined");
#endif
#ifdef WASM_ENABLE_JIT
    ESP_LOGI(TAG, "  ✅ WASM_ENABLE_JIT defined - JIT mode enabled");
#else
    ESP_LOGI(TAG, "  ❌ WASM_ENABLE_JIT not defined");
#endif
    
    // Load WASM module
    char error_buf[128];
    ESP_LOGI(TAG, "🔍 Calling wasm_runtime_load...");
    module->wasm_module = wasm_runtime_load(wasm_file_buf, file_size, error_buf, sizeof(error_buf));
    ESP_LOGI(TAG, "🔍 wasm_runtime_load returned: %p", module->wasm_module);
    
    if (!module->wasm_module) {
        ESP_LOGE(TAG, "Failed to load WASM module %s: %s", wasm_filename, error_buf);
        pthread_mutex_unlock(&g_mcp_modules_mutex);
        return NULL;
    }
    
    // Memory debugging before instantiation
    ESP_LOGI(TAG, "Memory before WASM instantiation: free=%zu bytes, largest_block=%zu bytes", 
             heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Instantiate WASM module with adequate memory for MCP operations
    uint32_t stack_size = 64 * 1024;  // Increased to 64KB to fix auxiliary stack overflow
    uint32_t heap_size = 256 * 1024;   // Increased from 4KB to 256KB for string operations and buffers
    module->wasm_module_inst = wasm_runtime_instantiate(module->wasm_module, stack_size, heap_size, error_buf, sizeof(error_buf));
    
    if (!module->wasm_module_inst) {
        ESP_LOGE(TAG, "Failed to instantiate WASM module %s: %s", wasm_filename, error_buf);
        ESP_LOGE(TAG, "Memory after failed instantiation: free=%zu bytes, largest_block=%zu bytes", 
                 heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        wasm_runtime_unload(module->wasm_module);
        pthread_mutex_unlock(&g_mcp_modules_mutex);
        return NULL;
    }
    
    // Create execution environment
    module->exec_env = wasm_runtime_create_exec_env(module->wasm_module_inst, stack_size);
    if (!module->exec_env) {
        ESP_LOGE(TAG, "Failed to create execution environment for %s", wasm_filename);
        wasm_runtime_deinstantiate(module->wasm_module_inst);
        wasm_runtime_unload(module->wasm_module);
        pthread_mutex_unlock(&g_mcp_modules_mutex);
        return NULL;
    }
    
    module->loaded = true;
    g_mcp_module_count++;
    
    ESP_LOGI(TAG, "Successfully loaded MCP WASM module: %s -> %s", route, wasm_filename);
    ESP_LOGI(TAG, "Memory after successful MCP module load: free=%zu bytes, largest_block=%zu bytes", 
             heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    mcp_memory_stats();
    
    // CRITICAL: Call _start() function to initialize the module and register tools
    ESP_LOGI(TAG, "🔍 DEBUG: Looking for _start function to initialize module");
    wasm_function_inst_t start_func = wasm_runtime_lookup_function(module->wasm_module_inst, "_start");
    if (start_func) {
        ESP_LOGI(TAG, "🔍 DEBUG: Found _start function, calling to register tools");
        uint32_t argv[1] = {0}; // _start takes no parameters
        bool start_success = wasm_runtime_call_wasm(module->exec_env, start_func, 0, argv);
        if (start_success) {
            ESP_LOGI(TAG, "✅ _start function called successfully - tools should be registered");
        } else {
            ESP_LOGW(TAG, "⚠️ _start function call failed - tools may not be registered");
            const char* exception = wasm_runtime_get_exception(module->wasm_module_inst);
            if (exception) {
                ESP_LOGW(TAG, "⚠️ _start exception: %s", exception);
            }
        }
    } else {
        ESP_LOGW(TAG, "⚠️ No _start function found - tools will not be registered automatically");
    }
    
    // Debug: Dump all WASM exports to diagnose function lookup issues
    mcp_wasm_dump_exports(module->wasm_module, wasm_filename);
    
    ESP_LOGI(TAG, "🔍 DEBUG: About to unlock mutex and test function lookup");
    ESP_LOGI(TAG, "🔍 DEBUG: g_mcp_module_count = %d", g_mcp_module_count);
    
    // Unlock mutex before function lookup test
    pthread_mutex_unlock(&g_mcp_modules_mutex);
    
    // Debug: test if the function lookup works here
    // Look for MCP request handler function
    ESP_LOGI(TAG, "🔍 DEBUG: Before lookup - module ptr: %p, wasm_module: %p, wasm_module_inst: %p", 
             module, module->wasm_module, module->wasm_module_inst);
    
    wasm_function_inst_t handle_request_func = wasm_runtime_lookup_function(module->wasm_module_inst, "handle_mcp_request");
    if (!handle_request_func) {
        ESP_LOGE(TAG, "WASM module %s missing handle_mcp_request function in load_mcp_wasm_module", wasm_filename);
        
        return NULL;
    }
    else {
        ESP_LOGI(TAG, "WASM module %s has handle_mcp_request function", wasm_filename);
        ESP_LOGI(TAG, "🔍 DEBUG: Function found at: %p", handle_request_func);
    }
    
    ESP_LOGI(TAG, "🔍 DEBUG: Returning module ptr: %p (array index: %d)", 
             module, (int)(module - g_mcp_modules));

    mcp_memory_stats();

    return module;
}

const char* mcp_wasm_find_file_for_route(const char* route) {
    // Check exact route match first
    for (int i = 0; i < MAX_MCP_ROUTES && g_mcp_config.routes[i].route[0] != '\0'; i++) {
        if (strcmp(g_mcp_config.routes[i].route, route) == 0) {
            return g_mcp_config.routes[i].wasm_file;
        }
    }
    
    // Check prefix match (e.g., "/mcp/led" matches "/mcp")
    for (int i = 0; i < MAX_MCP_ROUTES && g_mcp_config.routes[i].route[0] != '\0'; i++) {
        int route_len = strlen(g_mcp_config.routes[i].route);
        if (strncmp(g_mcp_config.routes[i].route, route, route_len) == 0 &&
            (route[route_len] == '/' || route[route_len] == '\0')) {
            return g_mcp_config.routes[i].wasm_file;
        }
    }
    
    return NULL;
}

void mcp_wasm_registry_stats(int* loaded_count, int* max_count) {
    pthread_mutex_lock(&g_mcp_modules_mutex);
    if (loaded_count) *loaded_count = g_mcp_module_count;
    if (max_count) *max_count = 8;
    pthread_mutex_unlock(&g_mcp_modules_mutex);
    mcp_memory_stats();
}

void mcp_wasm_registry_cleanup(void) {
    if (!g_registry_initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_mcp_modules_mutex);
    
    // Cleanup all loaded modules
    for (int i = 0; i < g_mcp_module_count; i++) {
        if (g_mcp_modules[i].loaded) {
            if (g_mcp_modules[i].watchdog) {
                esp_timer_stop((esp_timer_handle_t)g_mcp_modules[i].watchdog);
                esp_timer_delete((esp_timer_handle_t)g_mcp_modules[i].watchdog);
                g_mcp_modules[i].watchdog = NULL;
            }
            if (g_mcp_modules[i].exec_env) {
                wasm_runtime_destroy_exec_env(g_mcp_modules[i].exec_env);
            }
            if (g_mcp_modules[i].wasm_module_inst) {
                wasm_runtime_deinstantiate(g_mcp_modules[i].wasm_module_inst);
            }
            if (g_mcp_modules[i].wasm_module) {
                wasm_runtime_unload(g_mcp_modules[i].wasm_module);
            }
            if (g_mcp_modules[i].wasm_file_buf) {
                mcp_free(g_mcp_modules[i].wasm_file_buf);
                g_mcp_modules[i].wasm_file_buf = NULL;
                g_mcp_modules[i].wasm_file_size = 0;
            }
        }
    }
    
    memset(g_mcp_modules, 0, sizeof(g_mcp_modules));
    g_mcp_module_count = 0;
    g_registry_initialized = false;
    
    pthread_mutex_unlock(&g_mcp_modules_mutex);
    
    ESP_LOGI(TAG, "MCP module registry cleaned up");
}