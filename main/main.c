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

// MCP framework includes
#include "../mcp_esp32_framework/host/include/mcp_pthread_pool.h"
#include "../mcp_esp32_framework/host/include/mcp_wasm_runtime.h"

// WAMR includes
#include "wasm_export.h"
#include "bh_platform.h"

// MCP Framework includes
#include "../mcp_esp32_framework/host/include/mcp_config.h"
#include "../mcp_esp32_framework/host/include/mcp_config_loader.h"
#include "../mcp_esp32_framework/host/include/mcp_host_functions.h"
#include "../mcp_esp32_framework/host/include/mcp_http_server.h"
#include "../mcp_esp32_framework/host/include/mcp_wifi_minimal.h"
#include "../mcp_esp32_framework/host/include/mcp_wasm_runtime.h"
#include "../mcp_esp32_framework/host/include/mcp_session_manager.h"
// Note: mcp_types.h not needed yet - types defined in mcp_pthread_pool.h

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

static const char *TAG = "mcp-wamr";

// WiFi Configuration
// WIFI_SSID / WIFI_PASS come from .env at the project root (see .env.example)
#if !defined(WIFI_SSID) || !defined(WIFI_PASS)
#error "WIFI_SSID / WIFI_PASS not set - copy .env.example to .env"
#endif
#define WIFI_MAXIMUM_RETRY  5

// HTTP Server globals
static httpd_handle_t server = NULL;

// WiFi globals for framework (must match extern declarations in mcp_wifi_minimal.h)
EventGroupHandle_t g_wifi_event_group;
int g_s_retry_num = 0;
const int g_wifi_connected_bit = BIT0;
int g_wifi_maximum_retry = WIFI_MAXIMUM_RETRY;
const char* g_wifi_tag = "mcp-wamr";

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

// Control variable for LED blink loop
static bool g_led_blink_enabled = false;

// Forward declaration for WASM functions
static void register_mcp_endpoints(httpd_handle_t server);

// ========== MCP Config System ==========
// Configuration structures now defined in mcp_framework/include/mcp_config.h

// Global MCP configuration (accessible to framework)
mcp_config_t g_mcp_config = {0};
static bool g_mcp_config_loaded = false;

// ========== JSON Config Loading ==========
// JSON parsing functions moved to mcp_framework/src/mcp_config_loader.c

// ========== MCP Config Initialization ==========

// Initialize MCP configuration using framework functions
static bool init_mcp_config(void) {
    if (g_mcp_config_loaded) {
        return true;
    }
    
    // Use framework function with default fallback route
    if (!mcp_config_init_with_fallback(&g_mcp_config, "/mcp", "led_ctrl.was")) {
        ESP_LOGE(TAG, "Failed to initialize MCP configuration");
        return false;
    }
    
    g_mcp_config_loaded = true;
    
    // Register MCP endpoints now that config is loaded (if server is available)
    if (server) {
        register_mcp_endpoints(server);
    }
    
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

// WAMR allocators moved to mcp_host_functions framework

// ========== HTTP Server & SSE Functions ==========

// Send SSE event using MCP framework
static void send_sse_event(const char *event, const char *data) {
    // This function would need a global SSE connection reference
    // For now, just log - the framework SSE functions are available for real implementations
    ESP_LOGD(TAG, "SSE event (framework available): %s = %s", event, data);
    // TODO: Implement using mcp_sse_send_simple() with stored connection
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

// SSE handler moved to session manager - now accessible at /sse endpoint

// API endpoint to toggle LED via WASM
// MCP WASM Module Management moved to mcp_wasm_runtime framework

// Load WASM module for MCP route (moved to framework)
// Function moved to mcp_wasm_runtime framework as mcp_wasm_load_module_for_route()

// Function moved to mcp_wasm_runtime framework as mcp_wasm_find_file_for_route()



// Register MCP endpoints after config is loaded
static void register_mcp_endpoints(httpd_handle_t server) {
    if (!server) {
        ESP_LOGE(TAG, "HTTP server not available for MCP endpoint registration");
        return;
    }
    
    // Use framework function to register MCP endpoints
    esp_err_t ret = mcp_http_register_mcp_endpoints(server, &g_mcp_config, mcp_pthread_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MCP endpoints using framework: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "MCP endpoints registered successfully using framework");
    }
}



// Start HTTP server
static httpd_handle_t start_webserver(void) {
    // Configure HTTP server using MCP framework
    mcp_http_config_t http_config = MCP_HTTP_DEFAULT_CONFIG();
    http_config.port = 80;
    http_config.max_uri_handlers = 16;
    http_config.cors_enabled = true;
    
    // Start HTTP server using framework
    httpd_handle_t server = mcp_http_server_start(&http_config);
    if (!server) {
        ESP_LOGE(TAG, "Failed to start HTTP server using MCP framework");
        return NULL;
    }
    
    // Register project-specific handlers
    mcp_http_handler_t project_handlers[] = {
        {"/", HTTP_GET, root_handler, NULL},
        // {"/api/led/toggle", HTTP_POST, led_toggle_handler, NULL},
        {"/health", HTTP_GET, mcp_http_health_handler, NULL}  // Built-in framework handler
    };
    
    esp_err_t ret = mcp_http_register_handlers(server, project_handlers, 
                                               sizeof(project_handlers) / sizeof(project_handlers[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register project-specific handlers");
        mcp_http_server_stop(server);
        return NULL;
    }
    
    // Register SSE endpoint with session manager integration
    ret = mcp_http_register_sse_endpoint(server, "/sse");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register SSE endpoint");
        mcp_http_server_stop(server);
        return NULL;
    }
    
    return server;
}


// WiFi event handler - keep in main.c to avoid linking issues
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_s_retry_num < g_wifi_maximum_retry) {
            esp_wifi_connect();
            g_s_retry_num++;
            ESP_LOGI(TAG, "Retry connecting to WiFi...");
        } else {
            xEventGroupSetBits(g_wifi_event_group, BIT1);
        }
        ESP_LOGI(TAG,"WiFi connection failed");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        g_s_retry_num = 0;
        xEventGroupSetBits(g_wifi_event_group, g_wifi_connected_bit);
    }
}

// Initialize WiFi using minimal framework
static void wifi_init_sta(void) {
    ESP_LOGI(TAG, "Using minimal WiFi framework (exact copy of working code)...");
    mcp_wifi_init_sta(WIFI_SSID, WIFI_PASS, &wifi_event_handler);
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
    if (!mcp_wasm_mount_sd_card()) {
        ESP_LOGE(TAG, "Failed to mount SD card, cannot load WASM from SD");
        return;
        //goto no_wasm_fallback;
    }
    
    // Initialize MCP configuration system now that SD card is mounted
    if (!init_mcp_config()) {
        ESP_LOGE(TAG, "Failed to initialize MCP configuration");
        // Continue anyway with defaults
    } else {
        ESP_LOGI(TAG, "MCP configuration system initialized");
    }

    // Configure ESP32-specific memory allocation using framework allocators
    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_Allocator;
    
    // Get ESP32-specific allocators from framework
    void *malloc_func = NULL, *realloc_func = NULL, *free_func = NULL;
    mcp_get_wamr_allocators(&malloc_func, &realloc_func, &free_func);
    
    init_args.mem_alloc_option.allocator.malloc_func = malloc_func;
    init_args.mem_alloc_option.allocator.realloc_func = realloc_func;
    init_args.mem_alloc_option.allocator.free_func = free_func;
    
    // Register native symbols - use framework functions as the main set
    init_args.native_module_name = "env";
    init_args.n_native_symbols = mcp_core_host_functions_count;
    init_args.native_symbols = mcp_core_host_functions;
    
    ESP_LOGI(TAG, "Initializing WASM runtime with ESP32 allocator");
    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "WASM runtime full init failed.");
        return;
    }
    
    ESP_LOGI(TAG, "WASM runtime initialized successfully");
    
    // Initialize MCP pthread pool for concurrent request handling
    ESP_LOGI(TAG, "Initializing MCP pthread pool...");
    
    if (mcp_pthread_pool_init(g_mcp_config.system.max_wasm_modules) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MCP pthread pool");
        return;
    }
    
    // Thread environment should be automatically handled in pthread context
    ESP_LOGI(TAG, "Running in pthread context - thread environment should be automatic");    
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
    
    // Initialize MCP module registry
    ESP_LOGI(TAG, "Initializing MCP module registry...");
    if (!mcp_wasm_registry_init()) {
        ESP_LOGE(TAG, "Failed to initialize MCP module registry");
        return;
    }
    
    // Initialize MCP session manager
    ESP_LOGI(TAG, "Initializing MCP session manager...");
    ret = mcp_session_manager_init(NULL);  // Use default configuration
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MCP session manager: %s", esp_err_to_name(ret));
        return;
    }
    
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