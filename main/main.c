#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <sys/param.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

// MCP pthread handler
#include "mcp_pthread_handler.h"

// WAMR includes
#include "wasm_export.h"
#include "bh_platform.h"

// SD Card includes
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"

// HTTP Server includes
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/inet.h"

#define LED_GPIO GPIO_NUM_5
#define BLINK_PERIOD_MS 1000
#define IWASM_MAIN_STACK_SIZE 8192

static const char *TAG = "led-wamr";

// WiFi Configuration
#define WIFI_SSID      "your-ssid"
#define WIFI_PASS      "your-password"
#define WIFI_MAXIMUM_RETRY  5

// HTTP Server globals
static httpd_handle_t server = NULL;
static EventGroupHandle_t wifi_event_group;
static int s_retry_num = 0;
const int WIFI_CONNECTED_BIT = BIT0;

// SSE client tracking
#define MAX_SSE_CLIENTS 4
typedef struct {
    int fd;
    bool active;
} sse_client_t;
// static sse_client_t sse_clients[MAX_SSE_CLIENTS] = {0}; // Unused for now

// Global WASM runtime variables for HTTP API access
static wasm_module_t g_wasm_module = NULL;
static wasm_module_inst_t g_wasm_module_inst = NULL;
static wasm_exec_env_t g_exec_env = NULL;
static bool g_wasm_available = false;

// Global LED state for SSE integration
static volatile bool g_led_state = false;
static volatile bool g_led_state_changed = false;
static SemaphoreHandle_t g_led_state_mutex = NULL;

// Forward declaration for WASM functions
static bool safe_wasm_call(wasm_exec_env_t exec_env, wasm_function_inst_t func, uint32_t argc, uint32_t *argv);
static void register_mcp_endpoints(httpd_handle_t server);

// ========== MCP Config System ==========

#define MAX_PROTECTED_PINS 16
#define MAX_MCP_ROUTES 8
#define MAX_ROUTE_LENGTH 64
#define MAX_FILENAME_LENGTH 128

typedef struct {
    int protected_pins[MAX_PROTECTED_PINS];
    int protected_pin_count;
    int input_only_pins[MAX_PROTECTED_PINS];
    int input_only_pin_count;
    int max_current_pins[MAX_PROTECTED_PINS];
    int max_current_pin_count;
} gpio_protection_config_t;

typedef struct {
    char route[MAX_ROUTE_LENGTH];
    char wasm_file[MAX_FILENAME_LENGTH];
} mcp_route_config_t;

typedef struct {
    int max_wasm_modules;
    int wasm_heap_size;
    int wasm_stack_size;
    int sse_max_clients;
    int mcp_timeout_ms;
} system_config_t;

typedef struct {
    gpio_protection_config_t gpio_protection;
    mcp_route_config_t routes[MAX_MCP_ROUTES];
    int route_count;
    system_config_t system;
    bool require_auth;
    int rate_limit_per_minute;
} mcp_config_t;

// Global MCP configuration
static mcp_config_t g_mcp_config = {0};
static bool g_mcp_config_loaded = false;

// ========== Minimal JSON Parser ==========

// Find a string value in JSON by key
static bool json_find_string(const char* json, const char* key, char* value, size_t max_len) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    const char* key_pos = strstr(json, search_key);
    if (!key_pos) return false;
    
    // Find the opening quote of the value
    const char* value_start = strchr(key_pos + strlen(search_key), '"');
    if (!value_start) return false;
    value_start++; // Skip opening quote
    
    // Find the closing quote
    const char* value_end = strchr(value_start, '"');
    if (!value_end) return false;
    
    size_t len = value_end - value_start;
    if (len >= max_len) len = max_len - 1;
    
    strncpy(value, value_start, len);
    value[len] = '\0';
    return true;
}

// Find an integer value in JSON by key
static bool json_find_int(const char* json, const char* key, int* value) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    const char* key_pos = strstr(json, search_key);
    if (!key_pos) return false;
    
    // Skip to the value part
    const char* value_start = key_pos + strlen(search_key);
    while (*value_start == ' ' || *value_start == '\t') value_start++;
    
    *value = atoi(value_start);
    return true;
}

// Find a boolean value in JSON by key
static bool json_find_bool(const char* json, const char* key, bool* value) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    const char* key_pos = strstr(json, search_key);
    if (!key_pos) return false;
    
    const char* value_start = key_pos + strlen(search_key);
    while (*value_start == ' ' || *value_start == '\t') value_start++;
    
    if (strncmp(value_start, "true", 4) == 0) {
        *value = true;
        return true;
    } else if (strncmp(value_start, "false", 5) == 0) {
        *value = false;
        return true;
    }
    return false;
}

// Parse JSON array of integers
static int json_parse_int_array(const char* json, const char* key, int* array, int max_count) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":[", key);
    
    const char* array_start = strstr(json, search_key);
    if (!array_start) return 0;
    
    array_start += strlen(search_key);
    const char* array_end = strchr(array_start, ']');
    if (!array_end) return 0;
    
    int count = 0;
    const char* pos = array_start;
    
    while (pos < array_end && count < max_count) {
        while (*pos == ' ' || *pos == '\t' || *pos == ',') pos++;
        if (pos >= array_end) break;
        
        array[count] = atoi(pos);
        count++;
        
        // Find next comma or end
        while (pos < array_end && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }
    
    return count;
}

// ========== MCP Config Loader ==========

// Load MCP configuration from file
static bool load_mcp_config_from_file(const char* filepath) {
    ESP_LOGI(TAG, "Loading MCP config from: %s", filepath);
    
    FILE* file = fopen(filepath, "r");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open MCP config file: %s (errno: %d - %s)", filepath, errno, strerror(errno));
        return false;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (size <= 0 || size > 8192) {  // Reasonable config size limit
        ESP_LOGE(TAG, "Invalid MCP config file size: %ld", size);
        fclose(file);
        return false;
    }
    
    // Read entire config file
    char* config_json = malloc(size + 1);
    if (!config_json) {
        ESP_LOGE(TAG, "Failed to allocate memory for MCP config");
        fclose(file);
        return false;
    }
    
    size_t bytes_read = fread(config_json, 1, size, file);
    fclose(file);
    config_json[bytes_read] = '\0';
    
    if (bytes_read != size) {
        ESP_LOGE(TAG, "Failed to read MCP config completely");
        free(config_json);
        return false;
    }
    
    ESP_LOGI(TAG, "MCP config file loaded (%zu bytes)", bytes_read);
    
    // Parse GPIO protection settings
    g_mcp_config.gpio_protection.protected_pin_count = json_parse_int_array(
        config_json, "protected_pins", 
        g_mcp_config.gpio_protection.protected_pins, 
        MAX_PROTECTED_PINS
    );
    
    g_mcp_config.gpio_protection.input_only_pin_count = json_parse_int_array(
        config_json, "input_only_pins",
        g_mcp_config.gpio_protection.input_only_pins,
        MAX_PROTECTED_PINS
    );
    
    g_mcp_config.gpio_protection.max_current_pin_count = json_parse_int_array(
        config_json, "max_current_pins",
        g_mcp_config.gpio_protection.max_current_pins,
        MAX_PROTECTED_PINS
    );
    
    // Parse system settings
    json_find_int(config_json, "max_wasm_modules", &g_mcp_config.system.max_wasm_modules);
    json_find_int(config_json, "wasm_heap_size", &g_mcp_config.system.wasm_heap_size);
    json_find_int(config_json, "wasm_stack_size", &g_mcp_config.system.wasm_stack_size);
    json_find_int(config_json, "sse_max_clients", &g_mcp_config.system.sse_max_clients);
    json_find_int(config_json, "mcp_timeout_ms", &g_mcp_config.system.mcp_timeout_ms);
    
    // Parse security settings
    json_find_bool(config_json, "require_auth", &g_mcp_config.require_auth);
    json_find_int(config_json, "rate_limit_per_minute", &g_mcp_config.rate_limit_per_minute);
    
    // Parse routes from nested JSON structure
    g_mcp_config.route_count = 0;
    
    // Find the "routes" object in the JSON
    const char* routes_start = strstr(config_json, "\"routes\"");
    if (routes_start) {
        // Find the opening brace of the routes object
        const char* routes_obj = strchr(routes_start, '{');
        if (routes_obj) {
            routes_obj++; // Skip the opening brace
            
            // Parse individual route entries
            const char* route_patterns[] = {"\"/mcp\"", "\"/mcp/led\"", "\"/mcp/gpio\"", "\"/mcp/system\""};
            const char* route_names[] = {"/mcp", "/mcp/led", "/mcp/gpio", "/mcp/system"};
            
            for (int i = 0; i < 4 && g_mcp_config.route_count < MAX_MCP_ROUTES; i++) {
                const char* route_entry = strstr(routes_obj, route_patterns[i]);
                if (route_entry) {
                    // Find the value after the colon
                    const char* colon = strchr(route_entry, ':');
                    if (colon) {
                        colon++; // Skip colon
                        while (*colon == ' ' || *colon == '\t') colon++; // Skip whitespace
                        
                        if (*colon == '"') {
                            colon++; // Skip opening quote
                            char wasm_file[MAX_FILENAME_LENGTH];
                            int j = 0;
                            while (*colon && *colon != '"' && j < MAX_FILENAME_LENGTH - 1) {
                                wasm_file[j++] = *colon++;
                            }
                            wasm_file[j] = '\0';
                            
                            // Store the route
                            strncpy(g_mcp_config.routes[g_mcp_config.route_count].route, route_names[i], MAX_ROUTE_LENGTH - 1);
                            strncpy(g_mcp_config.routes[g_mcp_config.route_count].wasm_file, wasm_file, MAX_FILENAME_LENGTH - 1);
                            g_mcp_config.route_count++;
                            
                            ESP_LOGI(TAG, "Parsed route: %s -> %s", route_names[i], wasm_file);
                        }
                    }
                }
            }
        }
    }
    
    free(config_json);
    
    ESP_LOGI(TAG, "MCP config loaded successfully:");
    ESP_LOGI(TAG, "  Protected pins: %d", g_mcp_config.gpio_protection.protected_pin_count);
    ESP_LOGI(TAG, "  Input-only pins: %d", g_mcp_config.gpio_protection.input_only_pin_count);
    ESP_LOGI(TAG, "  Max current pins: %d", g_mcp_config.gpio_protection.max_current_pin_count);
    ESP_LOGI(TAG, "  Routes configured: %d", g_mcp_config.route_count);
    ESP_LOGI(TAG, "  Max WASM modules: %d", g_mcp_config.system.max_wasm_modules);
    ESP_LOGI(TAG, "  WASM heap size: %d", g_mcp_config.system.wasm_heap_size);
    
    return true;
}

// Initialize MCP configuration
static bool init_mcp_config(void) {
    if (g_mcp_config_loaded) {
        return true;
    }
    
    // Set defaults first
    g_mcp_config.system.max_wasm_modules = 4;
    g_mcp_config.system.wasm_heap_size = 8192;  // Reduced from 32KB to 8KB
    g_mcp_config.system.wasm_stack_size = 4096; // Reduced from 8KB to 4KB
    g_mcp_config.system.sse_max_clients = 8;
    g_mcp_config.system.mcp_timeout_ms = 5000;
    g_mcp_config.require_auth = false;
    g_mcp_config.rate_limit_per_minute = 100;
    
    // Try to load from SD card first
    ESP_LOGI(TAG, "Attempting to load MCP config from SD card...");
    if (load_mcp_config_from_file("/sdcard/cfg.jsn")) {
        ESP_LOGI(TAG, "MCP config loaded from SD card");
        g_mcp_config_loaded = true;
        
        // Register MCP endpoints now that config is loaded
        register_mcp_endpoints(server);
        
        return true;
    }
    
    // Also try without extension in case of FAT32 8.3 filename issues
    ESP_LOGI(TAG, "Trying alternative config filename...");
    if (load_mcp_config_from_file("/sdcard/mcp_config.json")) {
        ESP_LOGI(TAG, "MCP config loaded from SD card (full filename)");
        g_mcp_config_loaded = true;
        
        // Register MCP endpoints now that config is loaded
        register_mcp_endpoints(server);
        
        return true;
    }
    
    // Try to load from SPIFFS/NVS (embedded)
    if (load_mcp_config_from_file("/spiffs/cfg.jsn")) {
        ESP_LOGI(TAG, "MCP config loaded from embedded storage");
        g_mcp_config_loaded = true;
        return true;
    }
    
    ESP_LOGW(TAG, "No MCP config file found, using defaults");
    
    // Set up default routes
    strncpy(g_mcp_config.routes[0].route, "/mcp", MAX_ROUTE_LENGTH - 1);
    strncpy(g_mcp_config.routes[0].wasm_file, "default_mcp.wasm", MAX_FILENAME_LENGTH - 1);
    g_mcp_config.route_count = 1;
    
    // Set default protected pins (ESP32 boot/flash pins)
    int default_protected[] = {0, 1, 6, 7, 8, 9, 10, 11};
    int default_input_only[] = {34, 35, 36, 39};
    
    g_mcp_config.gpio_protection.protected_pin_count = 8;
    memcpy(g_mcp_config.gpio_protection.protected_pins, default_protected, sizeof(default_protected));
    
    g_mcp_config.gpio_protection.input_only_pin_count = 4;
    memcpy(g_mcp_config.gpio_protection.input_only_pins, default_input_only, sizeof(default_input_only));
    
    g_mcp_config_loaded = true;
    return true;
}

// Check if a GPIO pin is protected
// Note: These functions are currently unused but will be needed for future enhancements
#if 0
static bool is_gpio_pin_protected(int pin) {
    for (int i = 0; i < g_mcp_config.gpio_protection.protected_pin_count; i++) {
        if (g_mcp_config.gpio_protection.protected_pins[i] == pin) {
            return true;
        }
    }
    return false;
}

// Check if a GPIO pin is input-only
static bool is_gpio_pin_input_only(int pin) {
    for (int i = 0; i < g_mcp_config.gpio_protection.input_only_pin_count; i++) {
        if (g_mcp_config.gpio_protection.input_only_pins[i] == pin) {
            return true;
        }
    }
    return false;
}
#endif

// Include the generated WASM bytecode  
// #include "../led_controller_wasm.h"  // Rust WASM (193 bytes)
// #include "../simple_wasm.h"              // C WASM (497 bytes) - has init code
// #include "../minimal_wasm.h"             // Minimal C WASM (93 bytes) - no init
// #include "../led_wasm.h"                 // LED control WASM (218 bytes) - LED functions
// Now loading WASM from SD card instead of embedded header

#define MALLOC_ALIGN_SIZE 8

// ESP32-specific WAMR memory allocation functions (from ESP-WASMachine)
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

// ========== HTTP Server & SSE Functions ==========

// Send SSE event (currently disabled - using direct streaming approach)
static void send_sse_event(const char *event, const char *data) {
    // This function is temporarily disabled while we use the direct streaming approach
    ESP_LOGD(TAG, "SSE event would be sent: %s = %s", event, data);
    // TODO: Implement shared state mechanism for real-time LED events
}

// Root handler - simple status page
static esp_err_t root_handler(httpd_req_t *req) {
    const char *html = "<html><body>"
                      "<h1>ESP32 WASM LED Controller</h1>"
                      "<p>LED Status: <span id='status'>Unknown</span></p>"
                      "<button onclick='toggleLed()'>Toggle LED</button>"
                      "<script>"
                      "const evtSource = new EventSource('/events');"
                      "evtSource.addEventListener('led_status', (e) => {"
                      "  document.getElementById('status').textContent = e.data;"
                      "});"
                      "function toggleLed() {"
                      "  fetch('/api/led/toggle', {method: 'POST'});"
                      "}"
                      "</script>"
                      "</body></html>";
    
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

// SSE handler for event stream - sends real-time LED status
static esp_err_t sse_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "SSE client connecting...");
    
    // Set SSE headers
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Send initial welcome event
    const char *welcome = "event: connected\ndata: Connected to ESP32 LED Controller\n\n";
    if (httpd_resp_send_chunk(req, welcome, strlen(welcome)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send welcome SSE event");
        return ESP_FAIL;
    }
    
    // Send initial LED status
    if (g_led_state_mutex) {
        if (xSemaphoreTake(g_led_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            char led_event[64];
            snprintf(led_event, sizeof(led_event), 
                    "event: led_status\ndata: %s\n\n", 
                    g_led_state ? "ON" : "OFF");
            httpd_resp_send_chunk(req, led_event, strlen(led_event));
            xSemaphoreGive(g_led_state_mutex);
        }
    }
    
    ESP_LOGI(TAG, "SSE client connected, streaming LED status");
    
    // Stream LED status changes in real-time
    while (true) {
        bool state_changed = false;
        bool current_state = false;
        
        // Check if LED state has changed
        if (g_led_state_mutex) {
            if (xSemaphoreTake(g_led_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (g_led_state_changed) {
                    state_changed = true;
                    current_state = g_led_state;
                    g_led_state_changed = false; // Reset change flag
                }
                xSemaphoreGive(g_led_state_mutex);
            }
        }
        
        // Send LED status event if state changed
        if (state_changed) {
            char led_event[64];
            snprintf(led_event, sizeof(led_event), 
                    "event: led_status\ndata: %s\n\n", 
                    current_state ? "ON" : "OFF");
            
            if (httpd_resp_send_chunk(req, led_event, strlen(led_event)) != ESP_OK) {
                ESP_LOGW(TAG, "SSE client disconnected");
                break;
            }
            
            ESP_LOGI(TAG, "Sent LED status via SSE: %s", current_state ? "ON" : "OFF");
        }
        
        // Short delay to avoid busy waiting
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    // Close the chunked response
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "SSE connection closed");
    return ESP_OK;
}

// API endpoint to toggle LED via WASM
// MCP WASM Module Management - struct defined in mcp_pthread_handler.h

static mcp_wasm_module_t g_mcp_modules[8]; // Max 8 MCP modules
static int g_mcp_module_count = 0;
static pthread_mutex_t g_mcp_modules_mutex = PTHREAD_MUTEX_INITIALIZER;

// Debug function to dump all WASM module exports
static void dump_wasm_exports(wasm_module_t module, const char* module_name) {
    if (!module) {
        ESP_LOGW(TAG, "Cannot dump exports: module is NULL");
        return;
    }
    
    int32_t export_count = wasm_runtime_get_export_count(module);
    ESP_LOGI(TAG, "🔍 WASM Module '%s' has %d exports:", module_name, export_count);
    
    if (export_count <= 0) {
        ESP_LOGW(TAG, "  No exports found or error getting export count");
        return;
    }
    
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

// Load WASM module for MCP route
mcp_wasm_module_t* load_mcp_wasm_module(const char* route, const char* wasm_filename) {
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
    uint8_t *wasm_file_buf = malloc(file_size);
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
        free(wasm_file_buf);
        pthread_mutex_unlock(&g_mcp_modules_mutex);
        return NULL;
    }
    
    // Load WASM module
    char error_buf[128];
    module->wasm_module = wasm_runtime_load(wasm_file_buf, file_size, error_buf, sizeof(error_buf));
    free(wasm_file_buf);
    
    if (!module->wasm_module) {
        ESP_LOGE(TAG, "Failed to load WASM module %s: %s", wasm_filename, error_buf);
        pthread_mutex_unlock(&g_mcp_modules_mutex);
        return NULL;
    }
    
    // Unload LED WASM to free memory for MCP WASM
    if (g_wasm_module_inst && g_exec_env) {
        ESP_LOGI(TAG, "Unloading LED WASM to free memory for MCP WASM");
        size_t free_before_unload = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        
        wasm_runtime_destroy_exec_env(g_exec_env);
        wasm_runtime_deinstantiate(g_wasm_module_inst);
        wasm_runtime_unload(g_wasm_module);
        g_exec_env = NULL;
        g_wasm_module_inst = NULL;
        g_wasm_module = NULL;
        g_wasm_available = false;
        
        size_t free_after_unload = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "LED WASM unloaded: freed %zu bytes", free_after_unload - free_before_unload);
    }
    
    // Memory debugging before instantiation
    ESP_LOGI(TAG, "Memory before WASM instantiation: free=%zu bytes, largest_block=%zu bytes", 
             heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Instantiate WASM module with adequate memory for MCP operations
    uint32_t stack_size = 16384;  // Increased from 4KB to 16KB for MCP SDK operations
    uint32_t heap_size = 16384;   // Increased from 4KB to 16KB for string operations and buffers
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
    
    // Debug: Dump all WASM exports to diagnose function lookup issues
    dump_wasm_exports(module->wasm_module, wasm_filename);
    
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
    
    return module;
}

// Find WASM filename for route
const char* find_wasm_for_route(const char* route) {
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

// MCP HTTP Request Handler
static esp_err_t mcp_handler(httpd_req_t *req) {
    char content[2048];
    char response[2048];
    int ret, remaining = req->content_len;
    
    // Read request body
    if (remaining >= sizeof(content)) {
        ESP_LOGE(TAG, "MCP request too large: %d bytes", remaining);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }
    
    ret = httpd_req_recv(req, content, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request timeout");
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request");
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    ESP_LOGI(TAG, "MCP request to %s: %s", req->uri, content);
    
    // Find WASM module for this route
    const char* wasm_filename = find_wasm_for_route(req->uri);
    if (!wasm_filename) {
        ESP_LOGW(TAG, "No WASM module configured for route: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Route not configured");
        return ESP_FAIL;
    }
    
    // Load WASM module
    mcp_wasm_module_t* module = load_mcp_wasm_module(req->uri, wasm_filename);
    if (!module) {
        ESP_LOGE(TAG, "Failed to load WASM module for route: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load WASM module");
        return ESP_FAIL;
    }
    
    // Look for MCP request handler function
    wasm_function_inst_t handle_request_func = wasm_runtime_lookup_function(module->wasm_module_inst, "mcp_req");
    if (!handle_request_func) {
        ESP_LOGE(TAG, "WASM module %s missing mcp_req function", wasm_filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid WASM module");
        return ESP_FAIL;
    }
    
    // Allocate WASM memory for request and response
    uint32_t request_offset = wasm_runtime_module_malloc(module->wasm_module_inst, strlen(content) + 1, NULL);
    uint32_t response_offset = wasm_runtime_module_malloc(module->wasm_module_inst, sizeof(response), NULL);
    
    if (request_offset == 0 || response_offset == 0) {
        ESP_LOGE(TAG, "Failed to allocate WASM memory");
        if (request_offset) wasm_runtime_module_free(module->wasm_module_inst, request_offset);
        if (response_offset) wasm_runtime_module_free(module->wasm_module_inst, response_offset);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WASM memory allocation failed");
        return ESP_FAIL;
    }
    
    // Copy request to WASM memory
    void* request_ptr = wasm_runtime_addr_app_to_native(module->wasm_module_inst, request_offset);
    void* response_ptr = wasm_runtime_addr_app_to_native(module->wasm_module_inst, response_offset);
    strcpy((char*)request_ptr, content);
    
    // Call WASM function: mcp_req(request, response, max_len)
    uint32_t argv[3] = {request_offset, response_offset, sizeof(response)};
    
    bool call_success = wasm_runtime_call_wasm(module->exec_env, handle_request_func, 3, argv);
    int32_t result = argv[0];  // Return value
    
    // Copy response from WASM memory
    strcpy(response, (char*)response_ptr);
    
    // Free WASM memory
    wasm_runtime_module_free(module->wasm_module_inst, request_offset);
    wasm_runtime_module_free(module->wasm_module_inst, response_offset);
    
    if (!call_success || result != 0) {
        ESP_LOGE(TAG, "WASM MCP handler failed: result=%d", result);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WASM handler failed");
        return ESP_FAIL;
    }
    
    // Send JSON response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    
    ESP_LOGI(TAG, "MCP response: %s", response);
    return httpd_resp_send(req, response, strlen(response));
}

// Handle CORS preflight requests
static esp_err_t mcp_options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

// Register MCP endpoints after config is loaded
static void register_mcp_endpoints(httpd_handle_t server) {
    if (!server) {
        ESP_LOGE(TAG, "HTTP server not available for MCP endpoint registration");
        return;
    }
    
    ESP_LOGI(TAG, "Registering %d MCP endpoints...", g_mcp_config.route_count);
    
    for (int i = 0; i < g_mcp_config.route_count; i++) {
        // Register POST handler for MCP requests
        httpd_uri_t* mcp_post = malloc(sizeof(httpd_uri_t));
        mcp_post->uri = strdup(g_mcp_config.routes[i].route);
        mcp_post->method = HTTP_POST;
        mcp_post->handler = mcp_pthread_handler;
        mcp_post->user_ctx = NULL;
        httpd_register_uri_handler(server, mcp_post);
        
        // Register OPTIONS handler for CORS preflight
        httpd_uri_t* mcp_options = malloc(sizeof(httpd_uri_t));
        mcp_options->uri = strdup(g_mcp_config.routes[i].route);
        mcp_options->method = HTTP_OPTIONS;
        mcp_options->handler = mcp_options_handler;
        mcp_options->user_ctx = NULL;
        httpd_register_uri_handler(server, mcp_options);
        
        ESP_LOGI(TAG, "Registered MCP endpoint: %s -> %s", 
                 g_mcp_config.routes[i].route, 
                 g_mcp_config.routes[i].wasm_file);
    }
}

static esp_err_t led_toggle_handler(httpd_req_t *req) {
    const char *resp;
    
    if (g_wasm_available && g_exec_env && g_wasm_module_inst) {
        // Use WASM to toggle LED
        wasm_function_inst_t led_toggle_func = wasm_runtime_lookup_function(g_wasm_module_inst, "led_toggle");
        if (led_toggle_func) {
            uint32_t argv[1] = {0}; // argument for led_toggle function
            if (safe_wasm_call(g_exec_env, led_toggle_func, 1, argv)) {
                int led_result = argv[0];
                ESP_LOGI(TAG, "HTTP API: WASM LED toggle executed! LED %s", led_result ? "ON" : "OFF");
                resp = led_result ? "{\"led\":\"on\"}" : "{\"led\":\"off\"}";
                // SSE event already sent by native_led_control function
            } else {
                ESP_LOGE(TAG, "HTTP API: WASM LED toggle failed");
                resp = "{\"error\":\"WASM toggle failed\"}";
            }
        } else {
            ESP_LOGE(TAG, "HTTP API: WASM led_toggle function not found");
            resp = "{\"error\":\"WASM function not found\"}";
        }
    } else {
        // Fallback to direct GPIO if WASM not available
        static bool led_state = false;
        led_state = !led_state;
        gpio_set_level(LED_GPIO, led_state);
        send_sse_event("led_status", led_state ? "ON" : "OFF");
        resp = led_state ? "{\"led\":\"on\"}" : "{\"led\":\"off\"}";
        ESP_LOGW(TAG, "HTTP API: Using direct GPIO fallback, LED %s", led_state ? "ON" : "OFF");
    }
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// Start HTTP server
static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 16;  // Increase from default 8 to 16
    
    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root);
        
        httpd_uri_t events = {
            .uri       = "/events",
            .method    = HTTP_GET,
            .handler   = sse_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &events);
        
        httpd_uri_t led_toggle = {
            .uri       = "/api/led/toggle",
            .method    = HTTP_POST,
            .handler   = led_toggle_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &led_toggle);
        
        ESP_LOGI(TAG, "HTTP server started successfully");
        return server;
    }
    
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry connecting to WiFi...");
        } else {
            xEventGroupSetBits(wifi_event_group, BIT1);
        }
        ESP_LOGI(TAG,"WiFi connection failed");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize WiFi
static void wifi_init_sta(void) {
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi initialization finished");
    
    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | BIT1,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi SSID: %s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
    }
}

// ========== WASM Functions ==========

// Native function to control LED
static int32_t native_led_control(wasm_exec_env_t exec_env, int32_t state)
{
    ESP_LOGI(TAG, "WASM called native_led_control with state: %d", state);
    gpio_set_level(LED_GPIO, state ? 1 : 0);
    
    // Update global LED state for SSE integration
    if (g_led_state_mutex) {
        if (xSemaphoreTake(g_led_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool new_state = state ? true : false;
            if (g_led_state != new_state) {
                g_led_state = new_state;
                g_led_state_changed = true;
                ESP_LOGD(TAG, "LED state updated: %s", g_led_state ? "ON" : "OFF");
            }
            xSemaphoreGive(g_led_state_mutex);
        }
    }
    
    // Legacy SSE function (currently disabled)
    send_sse_event("led_status", state ? "ON" : "OFF");
    
    return state;
}

// Native function to get system tick
static int32_t native_get_tick(wasm_exec_env_t exec_env)
{
    return (int32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// MCP Host Function Interface Implementation

// GPIO Control Functions
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
    
    ESP_LOGI(TAG, "WASM setting GPIO %d to level %d", pin, level);
    esp_err_t err = gpio_set_level((gpio_num_t)pin, level);
    
    // Update LED state if this is the LED pin
    if (pin == LED_GPIO && g_led_state_mutex) {
        if (xSemaphoreTake(g_led_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool new_state = level ? true : false;
            if (g_led_state != new_state) {
                g_led_state = new_state;
                g_led_state_changed = true;
            }
            xSemaphoreGive(g_led_state_mutex);
        }
    }
    
    return err == ESP_OK ? 0 : -2;
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
        
        // Check if pin is input-only
        for (int i = 0; i < g_mcp_config.gpio_protection.input_only_pin_count; i++) {
            if (g_mcp_config.gpio_protection.input_only_pins[i] == pin && direction != GPIO_MODE_INPUT) {
                ESP_LOGW(TAG, "GPIO pin %d is input-only, output mode denied", pin);
                return -2;
            }
        }
    }
    
    esp_err_t err = gpio_set_direction((gpio_num_t)pin, (gpio_mode_t)direction);
    return err == ESP_OK ? 0 : -3;
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

// SSE Event Functions
static int32_t native_sse_send_event(wasm_exec_env_t exec_env, const char* event_type, const char* data)
{
    // Using automatic string conversion ($$) - no validation needed
    ESP_LOGI(TAG, "WASM sending SSE event: %s = %s", event_type, data);
    send_sse_event(event_type, data);
    return 0;
}

static int32_t native_sse_send_tool_result(wasm_exec_env_t exec_env, const char* tool_name, const char* result)
{
    // Using automatic string conversion ($$) - no validation needed
    
    // Create JSON-formatted tool result
    char event_data[512];
    snprintf(event_data, sizeof(event_data), "{\"tool\":\"%s\",\"result\":%s}", tool_name, result);
    send_sse_event("tool_result", event_data);
    return 0;
}

// System Info Functions
static int32_t native_get_tick_count(wasm_exec_env_t exec_env)
{
    return (int32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int32_t native_get_free_heap(wasm_exec_env_t exec_env)
{
    return (int32_t)esp_get_free_heap_size();
}

static int32_t native_get_uptime_ms(wasm_exec_env_t exec_env)
{
    return (int32_t)(esp_timer_get_time() / 1000);
}

// JSON Helper Functions
static int32_t native_json_parse_request(wasm_exec_env_t exec_env, const char* json, char* method, char* params, int32_t max_len)
{
    ESP_LOGI(TAG, "🔍 JSON_PARSE: Called with json=%p, method=%p, params=%p, max_len=%d", json, method, params, max_len);
    
    // Since we're using automatic conversion ($**i), the pointers are already native pointers
    // No validation needed - just direct access
    
    ESP_LOGI(TAG, "🔍 JSON_PARSE: All validations passed, JSON content: '%.50s'", json);
    
    // Parse JSON-RPC method
    ESP_LOGI(TAG, "🔍 JSON_PARSE: About to call json_find_string");
    if (!json_find_string(json, "method", method, 64)) {  // method buffer is 64 bytes
        ESP_LOGE(TAG, "🔍 JSON_PARSE: json_find_string failed - method not found");
        return -2; // Method not found
    }
    
    ESP_LOGI(TAG, "🔍 JSON_PARSE: Successfully parsed method: '%s'", method);
    
    // Parse JSON-RPC params (extract the params object as string)
    const char* params_start = strstr(json, "\"params\"");
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
                
                params[i] = c;
                i++;
                
                if (!in_string && brace_depth == 0 && (c == ']' || c == '"' || (c >= '0' && c <= '9'))) {
                    // End of simple value
                    i++;
                    break;
                }
            }
            params[i] = '\0';
        } else {
            strcpy(params, "null");
        }
    } else {
        strcpy(params, "null");
    }
    
    return 0;
}

static int32_t native_json_create_response(wasm_exec_env_t exec_env, const char* result, char* response, int32_t max_len)
{
    // Using automatic string conversion ($*i) - result is auto-converted, response is manual pointer
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_app_addr(module_inst, (uint32_t)response, max_len)) {
        return -1;
    }
    
    int len = snprintf(response, max_len,
        "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":1}",
        result);
    
    return len < max_len ? 0 : -2; // Buffer overflow
}

static int32_t native_json_create_error(wasm_exec_env_t exec_env, int32_t code, const char* message, char* response, int32_t max_len)
{
    // Using automatic string conversion (i$*i) - message is auto-converted, response is manual pointer
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_app_addr(module_inst, (uint32_t)response, max_len)) {
        return -1;
    }
    
    int len = snprintf(response, max_len,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%ld,\"message\":\"%s\"},\"id\":1}",
        (long)code, message);
    
    return len < max_len ? 0 : -2; // Buffer overflow
}

// Logging Functions
static int32_t native_log_info(wasm_exec_env_t exec_env, const char* tag, const char* message)
{
    //ESP_LOGI(TAG, "🔍 LOG_INFO: Called with tag=%p, message=%p", tag, message);
    
    // Since we're using automatic conversion ($**), the pointers are already native pointers
    // No validation needed - just direct access like json_parse_request
    
    //ESP_LOGI(TAG, "🔍 LOG_INFO: Tag content: '%.20s'", tag);
    //ESP_LOGI(TAG, "🔍 LOG_INFO: Message content: '%.50s'", message);
    
    ESP_LOGI(tag, "[WASM] %s", message);
    //ESP_LOGI(TAG, "🔍 LOG_INFO: Successfully logged message");
    return 0;
}

static int32_t native_log_error(wasm_exec_env_t exec_env, const char* tag, const char* message)
{
    // Using automatic string conversion ($$) - no validation needed
    ESP_LOGE(tag, "[WASM] %s", message);
    return 0;
}

// String Functions (C library compatibility)
static int32_t native_strcat(wasm_exec_env_t exec_env, char* dest, const char* src)
{
    // Using automatic string conversion ($$) - no validation needed
    
    // Standard strcat implementation
    char* d = dest;
    while (*d) d++; // Find end of dest
    while ((*d++ = *src++)); // Copy src to end of dest
    return (uint32_t)dest; // Return WASM address
}

// Native strcmp implementation
static int32_t native_strcmp(wasm_exec_env_t exec_env, const char* s1, const char* s2)
{
    ESP_LOGI("[WASM]", "strcmp called!");
    // Using automatic string conversion ($$) - no validation needed
    return strcmp(s1, s2);
}

// Native strlen implementation
static int32_t native_strlen(wasm_exec_env_t exec_env, const char* s)
{
    // Using automatic string conversion ($) - no validation needed
    return (int32_t)strlen(s);
}

// Native strncpy implementation
static int32_t native_strncpy(wasm_exec_env_t exec_env, char* dest, const char* src, int32_t n)
{
    // Using automatic string conversion ($*i) - src is auto-converted, dest is manual pointer
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_app_addr(module_inst, (uint32_t)dest, n)) {
        return 0; // Return 0 on validation failure
    }
    
    strncpy(dest, src, n);
    return (uint32_t)dest; // Return WASM address
}

// Native memcpy implementation  
static int32_t native_memcpy(wasm_exec_env_t exec_env, void* dest, const void* src, int32_t n)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_app_addr(module_inst, (uint32_t)dest, n) ||
        !wasm_runtime_validate_app_addr(module_inst, (uint32_t)src, n)) {
        return 0; // Return 0 on validation failure
    }
    
    memcpy(dest, src, n);
    return (uint32_t)dest; // Return WASM address
}

// Native memcmp implementation
static int32_t native_memcmp(wasm_exec_env_t exec_env, const void* s1, const void* s2, int32_t n)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_app_addr(module_inst, (uint32_t)s1, n) ||
        !wasm_runtime_validate_app_addr(module_inst, (uint32_t)s2, n)) {
        return 0; // Return 0 on validation failure
    }
    
    return memcmp(s1, s2, n);
}

// Native strstr implementation
static int32_t native_strstr(wasm_exec_env_t exec_env, const char* haystack, const char* needle)
{
    // Using automatic string conversion ($$) - no validation needed
    char* result = strstr(haystack, needle);
    return result ? (uint32_t)result : 0; // Return WASM address or 0 if not found
}

// Native strcpy implementation
static int32_t native_strcpy(wasm_exec_env_t exec_env, char* dest, const char* src)
{
    // Using automatic string conversion ($$) - no validation needed for src
    // dest is a pointer that we need to validate
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    // We can't easily validate dest size, so we'll trust the WASM code
    strcpy(dest, src);
    return (uint32_t)dest; // Return WASM address
}

// Native strncmp implementation
static int32_t native_strncmp(wasm_exec_env_t exec_env, const char* s1, const char* s2, unsigned long n)
{
    // Using automatic string conversion ($$) - no validation needed
    return strncmp(s1, s2, n);
}

// Native snprintf implementation (simplified for WASM compatibility)
static int32_t native_snprintf(wasm_exec_env_t exec_env, char* buffer, int32_t size, const char* format)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_app_addr(module_inst, (uint32_t)buffer, size) ||
        !wasm_runtime_validate_app_str_addr(module_inst, (uint32_t)format)) {
        return 0; // Return 0 on validation failure
    }
    
    // For now, implement a basic version that handles the MCP SDK's usage
    // This is a simplified version - real snprintf has variadic arguments
    return snprintf(buffer, size, "%s", format);
}

// Native symbol table - exports native functions to WASM
static NativeSymbol native_symbols[] = {
    // Legacy functions
    {"led_control", native_led_control, "(i)i", NULL},
    {"get_tick", native_get_tick, "()i", NULL},
    
    // GPIO Control
    {"gpio_set_level", native_gpio_set_level, "(ii)i", NULL},
    {"gpio_get_level", native_gpio_get_level, "(i)i", NULL},
    {"gpio_set_direction", native_gpio_set_direction, "(ii)i", NULL},
    {"gpio_is_pin_protected", native_gpio_is_pin_protected, "(i)i", NULL},
    
    // SSE Events
    {"sse_send_event", native_sse_send_event, "($$)i", NULL},
    {"sse_send_tool_result", native_sse_send_tool_result, "($$)i", NULL},
    
    // System Info
    {"get_tick_count", native_get_tick_count, "()i", NULL},
    {"get_free_heap", native_get_free_heap, "()i", NULL},
    {"get_uptime_ms", native_get_uptime_ms, "()i", NULL},
    
    // JSON Helper Functions
    {"json_parse_request", native_json_parse_request, "($**i)i", NULL},
    {"json_create_response", native_json_create_response, "($*i)i", NULL},
    {"json_create_error", native_json_create_error, "(i$*i)i", NULL},
    
    // Logging
    {"log_info", native_log_info, "($$)i", NULL},
    {"log_error", native_log_error, "($$)i", NULL},
    
    // String Functions (C library compatibility)  
    {"wasm_strcmp", native_strcmp, "($$)i", NULL},
    {"strlen", native_strlen, "($)i", NULL},
    {"strcat", native_strcat, "($$)i", NULL},
    {"strcpy", native_strcpy, "($*)i", NULL},
    {"strncpy", native_strncpy, "($*i)i", NULL},
    {"strncmp", native_strncmp, "($$i)i", NULL},
    {"memcpy", native_memcpy, "(**i)i", NULL},
    {"memcmp", native_memcmp, "(**i)i", NULL},
    {"strstr", native_strstr, "($$)i", NULL}
    // Note: snprintf removed - too complex for WAMR (variadic function)
};

// Helper function to create execution environment matching instantiation
static wasm_exec_env_t create_exec_env_minimal(wasm_module_inst_t module_inst) {
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

// SD card mount function - trying SPI mode for better reliability
static bool mount_sd_card(void) {
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
    slot_config.gpio_cs = GPIO_NUM_5;    // Chip Select
    slot_config.host_id = host.slot;
    
    ESP_LOGI(TAG, "Using SPI pins: CLK=18, MOSI=23, MISO=19, CS=5");
    
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
    
    // Initialize MCP configuration system now that SD card is mounted
    if (!init_mcp_config()) {
        ESP_LOGE(TAG, "Failed to initialize MCP configuration");
        // Continue anyway with defaults
    } else {
        ESP_LOGI(TAG, "MCP configuration system initialized");
    }
        return true;
    } else {
        ESP_LOGE(TAG, "❌ Both SPI and SDMMC modes failed: %s", esp_err_to_name(ret));
        ESP_LOGI(TAG, "Check SD card insertion, formatting (FAT32), and connections");
        return false;
    }
}

// Load WASM file from SD card
static uint8_t* load_wasm_from_sd(const char* filename, unsigned* file_size) {
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

// Safe WASM function call wrapper (returns false on pthread errors)
static bool safe_wasm_call(wasm_exec_env_t exec_env, wasm_function_inst_t func, uint32_t argc, uint32_t *argv) {
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


void iwasm_main(void *arg)
{
    ESP_LOGI(TAG, "Starting WAMR initialization");
    
    // Initialize pthread support properly - create a separate pthread task
    ESP_LOGI(TAG, "Running WAMR in main task (FreeRTOS context)");
    
    uint8_t *wasm_file_buf = NULL;
    unsigned wasm_file_buf_size = 0;
    wasm_module_t wasm_module = NULL;
    wasm_module_inst_t wasm_module_inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    char error_buf[128];
    RuntimeInitArgs init_args;
    bool wasm_available = false;

    // Mount SD card first
    if (!mount_sd_card()) {
        ESP_LOGE(TAG, "Failed to mount SD card, cannot load WASM from SD");
        goto no_wasm_fallback;
    }

    // Configure GPIO for LED
    gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_config);

    // Configure ESP32-specific memory allocation (from ESP-WASMachine)
    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func = wamr_malloc;
    init_args.mem_alloc_option.allocator.realloc_func = wamr_realloc;
    init_args.mem_alloc_option.allocator.free_func = wamr_free;
    
    // Register native symbols
    init_args.native_module_name = "env";
    init_args.n_native_symbols = sizeof(native_symbols) / sizeof(NativeSymbol);
    init_args.native_symbols = native_symbols;
    
    ESP_LOGI(TAG, "Initializing WASM runtime with ESP32 allocator");
    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "WASM runtime full init failed.");
        goto no_wasm_fallback;
    }
    
    ESP_LOGI(TAG, "WASM runtime initialized successfully");
    
    // Initialize MCP pthread pool for concurrent request handling
    ESP_LOGI(TAG, "Initializing MCP pthread pool...");
    if (mcp_pthread_pool_init(g_mcp_config.system.max_wasm_modules) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MCP pthread pool");
        goto no_wasm_fallback;
    }
    
    // Thread environment should be automatically handled in pthread context
    ESP_LOGI(TAG, "Running in pthread context - thread environment should be automatic");

    ESP_LOGI(TAG, "Loading WASM module from SD card");
    
    // Load WASM file from SD card instead of embedded header
    // Try the 8.3 FAT32 short filename first
    wasm_file_buf = load_wasm_from_sd("led.was", &wasm_file_buf_size);
    if (!wasm_file_buf) {
        // If that fails, try the long filename
        ESP_LOGI(TAG, "Short filename failed, trying long filename...");
        wasm_file_buf = load_wasm_from_sd("led_control.wasm", &wasm_file_buf_size);
    }
    if (!wasm_file_buf) {
        ESP_LOGE(TAG, "Failed to load WASM from SD card");
        goto fail1;
    }
    ESP_LOGI(TAG, "WASM module loaded from SD: %d bytes", wasm_file_buf_size);

    if (!(wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_buf_size,
                                          error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error in wasm_runtime_load: %s", error_buf);
        goto fail1;
    }

    ESP_LOGI(TAG, "Instantiating WASM runtime");
    ESP_LOGI(TAG, "Free heap before instantiate: %ld bytes", esp_get_free_heap_size());
    uint32_t stack_size = 4096;   // Stack size (from WASM compile)  
    uint32_t heap_size = 8192;   // Reduced from 32KB to 8KB to match working LED WASM
    
    if (!(wasm_module_inst = wasm_runtime_instantiate(wasm_module, 
                                                     stack_size,
                                                     heap_size,
                                                     error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error while instantiating: %s", error_buf);
        goto fail2;
    }
    
    ESP_LOGI(TAG, "WASM module instantiated successfully");
    ESP_LOGI(TAG, "Free heap after instantiate: %ld bytes", esp_get_free_heap_size());

    // Try creating exec env now that we have pthread context
    ESP_LOGI(TAG, "Creating execution environment with pthread context");
    exec_env = create_exec_env_minimal(wasm_module_inst);
    
    if (!exec_env) {
        ESP_LOGW(TAG, "Failed to create execution environment even with pthread context");
        ESP_LOGW(TAG, "Will continue with WASM loaded but cannot execute functions");
        ESP_LOGW(TAG, "Free heap: %ld bytes", esp_get_free_heap_size());
        // Continue execution but WASM functions won't work
    } else {
        ESP_LOGI(TAG, "✅ Execution environment created successfully with pthread context");
    }

    ESP_LOGI(TAG, "✅ WASM runtime initialized successfully!");
    wasm_available = true;
    
    // Set global variables for HTTP API access
    g_wasm_module = wasm_module;
    g_wasm_module_inst = wasm_module_inst;
    g_exec_env = exec_env;
    g_wasm_available = wasm_available;
    
    // Debug: Dump LED WASM exports for comparison
    dump_wasm_exports(wasm_module, "led.was");
    
    // Test WASM LED function execution in pthread context
    if (exec_env) {
        ESP_LOGI(TAG, "Testing WASM LED functions in pthread context...");
        
        // Test LED on function
        wasm_function_inst_t led_on_func = wasm_runtime_lookup_function(wasm_module_inst, "led_on");
        if (led_on_func) {
            if (safe_wasm_call(exec_env, led_on_func, 0, NULL)) {
                ESP_LOGI(TAG, "✅ WASM led_on() executed - LED should be ON");
                vTaskDelay(500 / portTICK_PERIOD_MS); // Show LED on for 500ms
            } else {
                ESP_LOGE(TAG, "❌ WASM led_on function call failed");
            }
        }
        
        // Test LED off function
        wasm_function_inst_t led_off_func = wasm_runtime_lookup_function(wasm_module_inst, "led_off");
        if (led_off_func) {
            if (safe_wasm_call(exec_env, led_off_func, 0, NULL)) {
                ESP_LOGI(TAG, "✅ WASM led_off() executed - LED should be OFF");
                vTaskDelay(500 / portTICK_PERIOD_MS); // Show LED off for 500ms
            } else {
                ESP_LOGE(TAG, "❌ WASM led_off function call failed");
            }
        }
    }

    goto main_loop;

no_wasm_fallback:
    ESP_LOGW(TAG, "WASM initialization failed, falling back to direct GPIO control");
    wasm_available = false;
    
    // Initialize MCP configuration with defaults (SD card failed)
    if (!init_mcp_config()) {
        ESP_LOGE(TAG, "Failed to initialize MCP configuration");
    } else {
        ESP_LOGI(TAG, "MCP configuration system initialized (defaults only)");
    }
    goto main_loop;

main_loop:    
    // LED blink loop with WASM integration or fallback
    bool led_state = false;
    int count = 0;
    
    if (g_wasm_available) {
        ESP_LOGI(TAG, "🔄 Starting WASM-controlled LED blink loop");
    } else {
        ESP_LOGI(TAG, "🔄 Starting direct GPIO LED blink loop");
    }
    
    while (1) {
        led_state = !led_state;
        
        if (g_wasm_available && g_exec_env && g_wasm_module_inst) {
            // Use WASM to control LED directly!
            ESP_LOGD(TAG, "Using WASM LED control functions");
            
            // Try to call the WASM LED toggle function
            wasm_function_inst_t led_toggle_func = wasm_runtime_lookup_function(g_wasm_module_inst, "led_toggle");
            if (led_toggle_func) {
                uint32_t argv[1] = {count};
                if (safe_wasm_call(g_exec_env, led_toggle_func, 1, argv)) {
                    int led_result = argv[0];
                    ESP_LOGI(TAG, "🟢 WASM LED toggle executed! LED %s (count: %d)", 
                            led_result ? "ON" : "OFF", count++);
                } else {
                    ESP_LOGE(TAG, "🔴 WASM LED toggle failed, using fallback");
                    gpio_set_level(LED_GPIO, led_state);
                    count++;
                }
            } else {
                // Fallback to basic LED on/off functions
                wasm_function_inst_t led_func = led_state ? 
                    wasm_runtime_lookup_function(g_wasm_module_inst, "led_on") :
                    wasm_runtime_lookup_function(g_wasm_module_inst, "led_off");
                    
                if (led_func) {
                    if (safe_wasm_call(g_exec_env, led_func, 0, NULL)) {
                        ESP_LOGI(TAG, "🟢 WASM LED %s executed (count: %d)", 
                                led_state ? "ON" : "OFF", count++);
                    } else {
                        ESP_LOGE(TAG, "🔴 WASM LED function failed, using fallback");
                        gpio_set_level(LED_GPIO, led_state);
                        count++;
                    }
                } else {
                    count++;
                    ESP_LOGI(TAG, "🔴 No WASM LED functions found, direct LED %s (count: %d)", 
                            led_state ? "ON" : "OFF", count);
                    gpio_set_level(LED_GPIO, led_state);
                }
            }
        } else if (wasm_available) {
            ESP_LOGI(TAG, "❌ WASM available but no exec env, using direct GPIO");
            count++;  
            ESP_LOGI(TAG, "🟡 WASM loaded (%d bytes), exec env failed, direct LED %s (count: %d)", 
                    wasm_file_buf_size, led_state ? "ON" : "OFF", count);
            gpio_set_level(LED_GPIO, led_state);
        } else {
            // Direct GPIO control fallback
            gpio_set_level(LED_GPIO, led_state);
            ESP_LOGI(TAG, "🔴 Fallback LED %s (count: %d)", led_state ? "ON" : "OFF", count++);
        }
        
        vTaskDelay(BLINK_PERIOD_MS / portTICK_PERIOD_MS);
    }

    // Cleanup (only reached if loop is broken)
    ESP_LOGI(TAG, "Deinitializing WASM runtime");
    if (exec_env) wasm_runtime_destroy_exec_env(exec_env);
    if (wasm_module_inst) wasm_runtime_deinstantiate(wasm_module_inst);
    wasm_runtime_destroy_thread_env();
    
fail2:
    if (wasm_module) wasm_runtime_unload(wasm_module);
    
fail1:
    if (wasm_file_buf) free(wasm_file_buf);
    wasm_runtime_destroy();
}

// Pthread entry point for WAMR
void* wamr_pthread_main(void* arg)
{
    ESP_LOGI(TAG, "WAMR pthread started");
    iwasm_main(arg);
    return NULL;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting LED + WAMR + HTTP Server application");
    
    // Initialize LED state mutex for SSE integration
    g_led_state_mutex = xSemaphoreCreateMutex();
    if (g_led_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LED state mutex");
        return;
    }
    ESP_LOGI(TAG, "LED state mutex initialized");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize WiFi
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_init_sta();
    
    // Start HTTP server
    server = start_webserver();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
    
    // Create pthread for ALL WAMR operations (like the reference code)
    pthread_t wamr_thread;
    pthread_attr_t attr;
    
    ESP_LOGI(TAG, "Creating pthread for WAMR (following reference pattern)...");
    
    if (pthread_attr_init(&attr) != 0) {
        ESP_LOGE(TAG, "Failed to init pthread attributes");
        return;
    }
    
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&attr, 32768);  // Larger stack like reference
    
    if (pthread_create(&wamr_thread, &attr, wamr_pthread_main, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to create WAMR pthread");
        return;
    }
    
    ESP_LOGI(TAG, "WAMR pthread created, waiting for completion...");
    
    void* thread_result;
    if (pthread_join(wamr_thread, &thread_result) != 0) {
        ESP_LOGE(TAG, "Failed to join WAMR pthread");
    }
    
    pthread_attr_destroy(&attr);
    ESP_LOGI(TAG, "WAMR pthread completed");
}