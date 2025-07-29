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
static sse_client_t sse_clients[MAX_SSE_CLIENTS] = {0};

// Global WASM runtime variables for HTTP API access
static wasm_module_inst_t g_wasm_module_inst = NULL;
static wasm_exec_env_t g_exec_env = NULL;
static bool g_wasm_available = false;

// Forward declaration for WASM functions
static bool safe_wasm_call(wasm_exec_env_t exec_env, wasm_function_inst_t func, uint32_t argc, uint32_t *argv);

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

// SSE handler for event stream - keeps connection alive
static esp_err_t sse_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "SSE client connecting...");
    
    // Set SSE headers
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Send initial welcome event
    const char *welcome = "event: connected\ndata: Connected to ESP32 SSE\n\n";
    if (httpd_resp_send_chunk(req, welcome, strlen(welcome)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send welcome SSE event");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "SSE client connected, starting event stream");
    
    // Keep connection alive and send periodic events
    int event_count = 0;
    while (true) {
        // Send current LED status (we'll determine this from WASM state)
        // For now, send a heartbeat every 5 seconds to test the connection
        char event_msg[128];
        snprintf(event_msg, sizeof(event_msg), 
                "event: heartbeat\ndata: Event %d at %lu ms\n\n", 
                event_count++, (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        
        if (httpd_resp_send_chunk(req, event_msg, strlen(event_msg)) != ESP_OK) {
            ESP_LOGW(TAG, "SSE client disconnected");
            break;
        }
        
        // Wait 5 seconds between heartbeat events
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        
        // Limit to prevent infinite loop in testing
        if (event_count > 100) {
            ESP_LOGI(TAG, "SSE test limit reached, closing connection");
            break;
        }
    }
    
    // Close the chunked response
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "SSE connection closed");
    return ESP_OK;
}

// API endpoint to toggle LED via WASM
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
    
    // Send SSE event when LED state changes
    send_sse_event("led_status", state ? "ON" : "OFF");
    
    return state;
}

// Native function to get system tick
static int32_t native_get_tick(wasm_exec_env_t exec_env)
{
    return (int32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Native symbol table - exports native functions to WASM
static NativeSymbol native_symbols[] = {
    {"led_control", native_led_control, "(i)i", NULL},
    {"get_tick", native_get_tick, "()i", NULL}
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
    
    // Thread environment should be automatically handled in pthread context
    ESP_LOGI(TAG, "Running in pthread context - thread environment should be automatic");

    ESP_LOGI(TAG, "Loading WASM module from SD card");
    
    // Load WASM file from SD card instead of embedded header
    // Try the 8.3 FAT32 short filename first
    wasm_file_buf = load_wasm_from_sd("LED_CO~1.WAS", &wasm_file_buf_size);
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
    uint32_t heap_size = 32768;  // Smaller heap size for pthread context
    
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
    g_wasm_module_inst = wasm_module_inst;
    g_exec_env = exec_env;
    g_wasm_available = wasm_available;
    
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
    goto main_loop;

main_loop:    
    // LED blink loop with WASM integration or fallback
    bool led_state = false;
    int count = 0;
    
    if (wasm_available) {
        ESP_LOGI(TAG, "🔄 Starting WASM-controlled LED blink loop");
    } else {
        ESP_LOGI(TAG, "🔄 Starting direct GPIO LED blink loop");
    }
    
    while (1) {
        led_state = !led_state;
        
        if (wasm_available && exec_env) {
            // Use WASM to control LED directly!
            ESP_LOGD(TAG, "Using WASM LED control functions");
            
            // Try to call the WASM LED toggle function
            wasm_function_inst_t led_toggle_func = wasm_runtime_lookup_function(wasm_module_inst, "led_toggle");
            if (led_toggle_func) {
                uint32_t argv[1] = {count};
                if (safe_wasm_call(exec_env, led_toggle_func, 1, argv)) {
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
                    wasm_runtime_lookup_function(wasm_module_inst, "led_on") :
                    wasm_runtime_lookup_function(wasm_module_inst, "led_off");
                    
                if (led_func) {
                    if (safe_wasm_call(exec_env, led_func, 0, NULL)) {
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