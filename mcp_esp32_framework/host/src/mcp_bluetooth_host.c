#include "mcp_bluetooth_host.h"
#include "mcp_host_functions.h"
#include "mcp_memory.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <math.h>


static const char* TAG = "BT_HOST";

// Global Bluetooth state
static bt_state_t g_bt_state = {0};

// Current scan filter type
static char g_scan_filter[16] = "all"; // Default to no filter

// Event group for synchronizing operations
static EventGroupHandle_t bt_event_group = NULL;
#define BT_SCAN_COMPLETE_BIT    BIT0
#define BT_PAIR_COMPLETE_BIT    BIT1
#define BT_CONNECT_COMPLETE_BIT BIT2

// A2DP streaming state and PSRAM buffers
static int16_t* a2dp_stream_buffer = NULL;
static size_t a2dp_buffer_size = 0;
static bool a2dp_streaming_active = false;
static float a2dp_stream_time = 0.0f;

// A2DP melody state tracking
typedef struct {
    float frequency;
    int duration_ms;
} a2dp_note_t;

static a2dp_note_t* a2dp_melody_notes = NULL;
static int a2dp_melody_count = 0;
static int a2dp_current_note_index = 0;
static int64_t a2dp_note_start_time_us = 0;
static int a2dp_melody_volume = 50;
static bool a2dp_melody_playing = false;

// A2DP streaming configuration
#define A2DP_SAMPLE_RATE 44100
#define A2DP_BUFFER_FRAMES 1024
#define A2DP_BUFFER_SIZE (A2DP_BUFFER_FRAMES * 2 * sizeof(int16_t)) // Stereo

// Sine lookup table for efficient audio generation
#define SINE_TABLE_SIZE 1024
#define SINE_TABLE_SHIFT 10  // log2(SINE_TABLE_SIZE)
static int16_t* sine_lookup_table = NULL;

// Audio generation state for phase continuity
static float a2dp_phase = 0.0f;  // Current phase (0.0 to 1.0)

// Forward declarations for A2DP streaming functions
static int32_t a2dp_init_streaming_buffer(void);
static void a2dp_cleanup_streaming_buffer(void);
static int32_t a2dp_audio_callback(uint8_t* data, int32_t len);

// Forward declarations for A2DP melody functions
static float a2dp_note_to_frequency(const char* note);
static int32_t a2dp_parse_melody(const char* melody_str);
static void a2dp_cleanup_melody(void);
static void a2dp_start_melody(void);
static void a2dp_stop_melody(void);

// Forward declarations for sine lookup table
static int32_t a2dp_init_sine_table(void);
static void a2dp_cleanup_sine_table(void);
static inline int16_t a2dp_sine_lookup(float phase);

// Initialize Bluetooth stack
int32_t bluetooth_init(wasm_exec_env_t exec_env) {
    ESP_LOGI(TAG, "Initializing Bluetooth stack");
    
    if (g_bt_state.initialized) {
        ESP_LOGW(TAG, "Bluetooth already initialized");
        return 0;
    }
    
    esp_err_t ret;
    
    // Initialize event group
    bt_event_group = xEventGroupCreate();
    if (!bt_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return -1;
    }
    
    // Initialize NVS for Bluetooth (required for pairing info storage)
    // Note: This assumes NVS was already initialized in main()
    
    // Initialize and enable Bluetooth controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    
    // Configure for Classic Bluetooth only (A2DP) to reduce memory usage
    bt_cfg.mode = ESP_BT_MODE_CLASSIC_BT;
    
    // Memory optimizations for Classic BT only
    // bt_cfg.ble_max_conn = 0;                    // No BLE connections needed
    bt_cfg.bt_max_acl_conn = 1;                 // Only need 1 ACL connection for A2DP
    // bt_cfg.bt_max_sync_conn = 0;                // No SCO connections needed for A2DP
    // bt_cfg.normal_adv_size = 0;                 // No BLE advertising
    // bt_cfg.mesh_adv_size = 0;                   // No mesh support
    // bt_cfg.controller_task_stack_size = 2048;   // Reduce controller task stack (default ~4KB)
    
    ESP_LOGI(TAG, "Bluetooth controller config: Classic only, 1 ACL conn, %d stack", bt_cfg.controller_task_stack_size);
    
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(ret));
        return -2;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return -3;
    }
    
    // Initialize Bluedroid stack
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return -4;
    }
    
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return -5;
    }
    
    // Register GAP callback for device discovery and pairing
    ret = esp_bt_gap_register_callback(bt_gap_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP register callback failed: %s", esp_err_to_name(ret));
        return -6;
    }
    
    // Initialize A2DP source (to stream music TO speakers)
    ret = esp_a2d_register_callback(bt_a2dp_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP register callback failed: %s", esp_err_to_name(ret));
        return -7;
    }
    
    ret = esp_a2d_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP source init failed: %s", esp_err_to_name(ret));
        return -8;
    }
    
    // Initialize AVRC controller
    ret = esp_avrc_ct_register_callback(bt_avrc_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVRC register callback failed: %s", esp_err_to_name(ret));
        return -9;
    }
    
    ret = esp_avrc_ct_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVRC init failed: %s", esp_err_to_name(ret));
        return -10;
    }
    
    // Configure SSP (Secure Simple Pairing) for "Just Works" mode
    ESP_LOGI(TAG, "Configuring SSP for 'Just Works' mode");
    
    // Set IO capability to NONE for "Just Works" - no display/input needed
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));
    
    // Note: ESP-IDF Classic BT doesn't have separate auth_req settings for SSP
    // The IO capability setting (ESP_BT_IO_CAP_NONE) is sufficient for Just Works mode
    
    // Keep legacy PIN as fallback for older devices
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
    esp_bt_gap_set_pin(pin_type, 4, pin_code);
    ESP_LOGI(TAG, "SSP configured with fallback PIN: 1234");
    
    // Set device name for proper identification during pairing
    const char* device_name = "ESP32-MusicPlayer";
    ret = esp_bt_gap_set_device_name(device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set device name: %s", esp_err_to_name(ret));
        return -11;
    }
    ESP_LOGI(TAG, "Bluetooth device name set to: %s", device_name);
    
    // Set default GAP parameters
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    
    // Initialize state
    g_bt_state.initialized = true;
    g_bt_state.state = BT_STATE_IDLE;
    g_bt_state.discovered_device_count = 0;
    g_bt_state.a2dp_connected = false;
    g_bt_state.current_volume = 50; // Default volume
    memset(g_bt_state.connected_device_name, 0, sizeof(g_bt_state.connected_device_name));
    
    ESP_LOGI(TAG, "Bluetooth stack initialized successfully");
    return 0;
}

// Deinitialize Bluetooth stack
int32_t bluetooth_deinit(wasm_exec_env_t exec_env) {
    ESP_LOGI(TAG, "Deinitializing Bluetooth stack");
    
    if (!g_bt_state.initialized) {
        return 0;
    }
    
    // Disconnect if connected
    if (g_bt_state.state == BT_STATE_CONNECTED) {
        bluetooth_disconnect(exec_env);
    }
    
    // Stop and cleanup A2DP streaming
    a2dp_streaming_active = false;
    a2dp_cleanup_streaming_buffer();
    a2dp_cleanup_sine_table();
    a2dp_cleanup_melody();
    
    // Deinitialize components in reverse order
    esp_avrc_ct_deinit();
    esp_a2d_source_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    
    // Clean up event group
    if (bt_event_group) {
        vEventGroupDelete(bt_event_group);
        bt_event_group = NULL;
    }
    
    // Reset state
    memset(&g_bt_state, 0, sizeof(g_bt_state));
    
    ESP_LOGI(TAG, "Bluetooth stack deinitialized");
    return 0;
}

// Start device scanning
int32_t bluetooth_scan_start(wasm_exec_env_t exec_env, int32_t timeout_sec) {
    ESP_LOGI(TAG, "Starting device scan for %d seconds", timeout_sec);
    
    // For now, hardcode to audio filter - will add parameter later
    const char* filter_type = "audio";
    ESP_LOGI(TAG, "Using device filter: %s", filter_type);
    
    if (!g_bt_state.initialized) {
        ESP_LOGE(TAG, "Bluetooth not initialized");
        return -1;
    }
    
    if (g_bt_state.state == BT_STATE_SCANNING) {
        ESP_LOGW(TAG, "Scan already in progress");
        return -2;
    }
    
    // Limit timeout to reasonable range
    if (timeout_sec < 1 || timeout_sec > 30) {
        timeout_sec = 10;
    }
    
    // Clear previous scan results
    g_bt_state.discovered_device_count = 0;
    memset(g_bt_state.discovered_devices, 0, sizeof(g_bt_state.discovered_devices));
    
    // Set scan filter for callback filtering
    strncpy(g_scan_filter, filter_type, sizeof(g_scan_filter) - 1);
    g_scan_filter[sizeof(g_scan_filter) - 1] = '\0';
    
    // Clear scan complete bit
    xEventGroupClearBits(bt_event_group, BT_SCAN_COMPLETE_BIT);
    
    // Set scanning state
    g_bt_state.state = BT_STATE_SCANNING;
    
    // Note: ESP-IDF inquiry filter APIs not available in Classic BT mode
    // Will filter in callback instead for same memory benefit
    ESP_LOGI(TAG, "Audio filtering will be applied in discovery callback");
    
    // Start discovery
    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, timeout_sec, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start discovery failed: %s", esp_err_to_name(ret));
        g_bt_state.state = BT_STATE_IDLE;
        return -3;
    }
    
    // Wait for scan to complete
    EventBits_t bits = xEventGroupWaitBits(bt_event_group, BT_SCAN_COMPLETE_BIT, 
                                          pdTRUE, pdFALSE, pdMS_TO_TICKS((timeout_sec + 2) * 1000));
    
    if (!(bits & BT_SCAN_COMPLETE_BIT)) {
        ESP_LOGW(TAG, "Scan timeout");
        esp_bt_gap_cancel_discovery();
    }
    
    g_bt_state.state = BT_STATE_IDLE;
    ESP_LOGI(TAG, "Scan completed. Found %d devices", g_bt_state.discovered_device_count);
    
    return 0;
}

// Get scan results as JSON
int32_t bluetooth_scan_get_results(wasm_exec_env_t exec_env, char* buffer, int32_t max_len) {
    ESP_LOGI(TAG, "bluetooth_scan_get_results: START - buffer=%p, max_len=%d", buffer, max_len);
    
    if (!buffer || max_len < 1) {
        ESP_LOGE(TAG, "bluetooth_scan_get_results: Invalid parameters - buffer=%p, max_len=%d", buffer, max_len);
        return -1;
    }
    
    if (!g_bt_state.initialized) {
        ESP_LOGW(TAG, "bluetooth_scan_get_results: Bluetooth not initialized");
        snprintf(buffer, max_len, "{\"error\":\"Bluetooth not initialized\"}");
        return -1;
    }
    
    ESP_LOGI(TAG, "bluetooth_scan_get_results: Found %d devices to report", g_bt_state.discovered_device_count);
    
    // Start JSON array
    int offset = snprintf(buffer, max_len, "{\"devices\":[");
    ESP_LOGI(TAG, "bluetooth_scan_get_results: Initial offset=%d, max_len=%d", offset, max_len);
    
    if (offset >= max_len - 1) {
        ESP_LOGE(TAG, "bluetooth_scan_get_results: Buffer too small for header - offset=%d, max_len=%d", offset, max_len);
        return -2;
    }
    
    for (int i = 0; i < g_bt_state.discovered_device_count && offset < max_len - 200; i++) {
        bt_discovered_device_t* dev = &g_bt_state.discovered_devices[i];
        
        ESP_LOGI(TAG, "bluetooth_scan_get_results: Device %d/%d - name='%s', addr=%p", 
                 i+1, g_bt_state.discovered_device_count, 
                 dev->name[0] ? dev->name : "Unknown", 
                 (void*)dev);
        
        // Check remaining space
        if (max_len - offset < 150) {
            ESP_LOGW(TAG, "bluetooth_scan_get_results: Not enough space for device %d, stopping early", i);
            break;
        }
        
        // Add comma for subsequent entries
        if (i > 0) {
            int comma_len = snprintf(buffer + offset, max_len - offset, ",");
            offset += comma_len;
            ESP_LOGD(TAG, "bluetooth_scan_get_results: Added comma, new offset=%d", offset);
        }
        
        // Get device address string
        const char* addr_str = bt_addr_to_string(dev->addr);
        ESP_LOGI(TAG, "bluetooth_scan_get_results: Device address string: %s", addr_str);
        
        // Add device entry
        int dev_len = snprintf(buffer + offset, max_len - offset,
            "{\"name\": \"%s\", \"address\": \"%s\", \"rssi\": %"PRId32", \"audio_device\": %s}",
            dev->name[0] ? dev->name : "Unknown",
            addr_str,
            dev->rssi,
            dev->is_audio_device ? "true" : "false"
        );
        
        ESP_LOGI(TAG, "bluetooth_scan_get_results: Device %d JSON length=%d, new offset=%d", i, dev_len, offset + dev_len);
        
        if (dev_len >= max_len - offset) {
            ESP_LOGE(TAG, "bluetooth_scan_get_results: Device %d truncated - dev_len=%d, remaining=%d", 
                     i, dev_len, max_len - offset);
            break;
        }
        
        offset += dev_len;
    }
    
    ESP_LOGI(TAG, "bluetooth_scan_get_results: Closing JSON, offset=%d, remaining=%d", offset, max_len - offset);
    
    // Close JSON
    int close_len = snprintf(buffer + offset, max_len - offset, "],\"count\":%d}", 
                            g_bt_state.discovered_device_count);
    
    if (close_len >= max_len - offset) {
        ESP_LOGE(TAG, "bluetooth_scan_get_results: Buffer overflow on close - close_len=%d, remaining=%d", 
                 close_len, max_len - offset);
        buffer[max_len - 1] = '\0';  // Ensure null termination
        return -3;
    }
    
    offset += close_len;
    ESP_LOGI(TAG, "bluetooth_scan_get_results: SUCCESS - Final JSON length=%d", offset);
    ESP_LOGD(TAG, "bluetooth_scan_get_results: JSON output: %.100s...", buffer);  // First 100 chars
    
    return 0;
}

// Check if connected
int32_t bluetooth_is_connected(wasm_exec_env_t exec_env) {
    return g_bt_state.state == BT_STATE_CONNECTED ? 1 : 0;
}

// Get connected device name
int32_t bluetooth_get_connected_device_name(wasm_exec_env_t exec_env, char* buffer, int32_t max_len) {
    if (!buffer || max_len < 1) {
        return -1;
    }
    
    if (g_bt_state.state != BT_STATE_CONNECTED) {
        buffer[0] = '\0';
        return -1;
    }
    
    strncpy(buffer, g_bt_state.connected_device_name, max_len - 1);
    buffer[max_len - 1] = '\0';
    
    return 0;
}

// Utility: Convert Bluetooth address to string
const char* bt_addr_to_string(esp_bd_addr_t addr) {
    static char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return addr_str;
}

// Find device by name in discovered devices
int32_t bt_find_device_by_name(const char* device_name) {
    if (!device_name) return -1;
    
    for (int i = 0; i < g_bt_state.discovered_device_count; i++) {
        if (strstr(g_bt_state.discovered_devices[i].name, device_name) != NULL) {
            return i;
        }
    }
    return -1;
}

// GAP callback for device discovery and pairing events
void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        // Device discovered during scan
        esp_bt_gap_dev_prop_t *prop = NULL;
        char device_name[MAX_DEVICE_NAME_LEN] = {0};
        bool is_audio_device = false;
        int32_t rssi = -128; // Default value if not found
        uint8_t *eir_data = NULL;
        int eir_len = 0;
        
        // Check if we have room for more devices
        if (g_bt_state.discovered_device_count >= MAX_DISCOVERED_DEVICES) {
            break;
        }
        
        // Early duplicate check to reduce processing overhead
        bool is_duplicate = false;
        for (int i = 0; i < g_bt_state.discovered_device_count; i++) {
            if (memcmp(g_bt_state.discovered_devices[i].addr, param->disc_res.bda, ESP_BD_ADDR_LEN) == 0) {
                ESP_LOGD(TAG, "Ignoring duplicate device callback: %s", 
                         bt_addr_to_string(param->disc_res.bda));
                is_duplicate = true;
                break;
            }
        }
        
        if (is_duplicate) {
            break; // Skip all processing for duplicates
        }
        
        // Extract device properties
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            prop = param->disc_res.prop + i;
            switch (prop->type) {
            case ESP_BT_GAP_DEV_PROP_BDNAME:
                memcpy(device_name, prop->val, prop->len);
                device_name[prop->len] = '\0';
                break;
            case ESP_BT_GAP_DEV_PROP_COD: {
                uint32_t cod = *(uint32_t *)(prop->val);
                
                // Use proper ESP-IDF helper functions for CoD analysis
                uint32_t service_class = esp_bt_gap_get_cod_srvc(cod);
                uint32_t major_dev = esp_bt_gap_get_cod_major_dev(cod);
                
                // Follow ESP-IDF example: validate CoD and check for audio-capable devices
                // Include PHONE devices (many phones support A2DP) and AV devices
                bool is_valid_cod = esp_bt_gap_is_valid_cod(cod);
                bool is_phone_device = (major_dev == ESP_BT_COD_MAJOR_DEV_PHONE);
                bool is_av_device = (major_dev == ESP_BT_COD_MAJOR_DEV_AV);  
                bool has_audio_service = (service_class & ESP_BT_COD_SRVC_AUDIO) != 0;
                
                // Audio device if: valid CoD AND (phone OR A/V device OR has audio service)
                is_audio_device = is_valid_cod && (is_phone_device || is_av_device || has_audio_service);
                
                // Debug logging for CoD analysis
                // ESP_LOGI(TAG, "CoD: 0x%06X, Svc:0x%03X, Major:%d, Valid:%s, Phone:%s, AV:%s, AudioSvc:%s, Final:%s",
                //          cod, service_class, major_dev,
                //          is_valid_cod ? "Y" : "N",
                //          is_phone_device ? "Y" : "N", 
                //          is_av_device ? "Y" : "N",
                //          has_audio_service ? "Y" : "N",
                //          is_audio_device ? "YES" : "NO");
                break;
            }
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *)(prop->val);
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                eir_len = prop->len;
                eir_data = (uint8_t *)(prop->val);
                ESP_LOGD(TAG, "EIR data found: length=%d", eir_len);
                break;
            default:
                break;
            }
        }
        
        // Extract name from EIR data if direct name wasn't available (following ESP-IDF example)
        if (device_name[0] == '\0' && eir_data && eir_len > 0) {
            uint8_t *rmt_bdname = NULL;
            uint8_t rmt_bdname_len = 0;
            
            // Try complete local name first
            rmt_bdname = esp_bt_gap_resolve_eir_data(eir_data, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
            if (!rmt_bdname) {
                // Fall back to short local name
                rmt_bdname = esp_bt_gap_resolve_eir_data(eir_data, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
            }
            
            if (rmt_bdname && rmt_bdname_len > 0) {
                // Limit name length to our buffer
                if (rmt_bdname_len >= MAX_DEVICE_NAME_LEN) {
                    rmt_bdname_len = MAX_DEVICE_NAME_LEN - 1;
                }
                memcpy(device_name, rmt_bdname, rmt_bdname_len);
                device_name[rmt_bdname_len] = '\0';
                ESP_LOGI(TAG, "Extracted name from EIR: '%s' (len=%d)", device_name, rmt_bdname_len);
            } else {
                ESP_LOGD(TAG, "No name found in EIR data");
            }
        }
        
        // Apply scan filter - skip non-matching devices to save memory
        if (strcmp(g_scan_filter, "audio") == 0) {
            if (!is_audio_device) {
                ESP_LOGD(TAG, "Skipping non-audio device: %s (%s)", 
                         device_name[0] ? device_name : "Unknown", 
                         bt_addr_to_string(param->disc_res.bda));
                break; // Skip this device - not stored, saves memory
            }
            ESP_LOGI(TAG, "Audio device detected: %s", device_name[0] ? device_name : "Unknown");
        }
        
        // Store discovered device (only reaches here if passed filter and not duplicate)
        bt_discovered_device_t* dev = &g_bt_state.discovered_devices[g_bt_state.discovered_device_count];
        memcpy(dev->addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
        strncpy(dev->name, device_name, sizeof(dev->name) - 1);
        dev->name[sizeof(dev->name) - 1] = '\0';
        dev->rssi = rssi;
        dev->is_audio_device = is_audio_device;
        
        g_bt_state.discovered_device_count++;
        
        ESP_LOGI(TAG, "Device found: %s (%s) RSSI:%"PRId32" Audio:%s",
                 device_name, bt_addr_to_string(dev->addr), dev->rssi,
                 is_audio_device ? "Yes" : "No");
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        // Discovery state changed
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Device discovery stopped");
            xEventGroupSetBits(bt_event_group, BT_SCAN_COMPLETE_BIT);
        }
        break;
    
    // SSP (Secure Simple Pairing) Events
    case ESP_BT_GAP_CFM_REQ_EVT:
        // SSP Numeric Comparison or Just Works - auto-confirm for Just Works mode
        ESP_LOGI(TAG, "SSP Confirmation Request - Auto confirming (Just Works mode)");
        ESP_LOGI(TAG, "Device: %s, Numeric Value: %"PRIu32, 
                 bt_addr_to_string(param->cfm_req.bda), param->cfm_req.num_val);
        
        // Always confirm for "Just Works" mode
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        // SSP Passkey Notification
        ESP_LOGI(TAG, "SSP Passkey Notification: %06"PRIu32, param->key_notif.passkey);
        // No action needed - just for user information
        break;
    
    case ESP_BT_GAP_KEY_REQ_EVT:
        // Legacy pairing or SSP passkey request
        ESP_LOGI(TAG, "SSP Passkey Request - device may not support Just Works");
        // This shouldn't happen with Just Works, but handle it anyway
        // Could implement passkey entry here if needed
        break;
    
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        // Authentication/Pairing complete
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "✅ Authentication SUCCESS with %s", 
                     bt_addr_to_string(param->auth_cmpl.bda));
            ESP_LOGI(TAG, "Device name: %s", param->auth_cmpl.device_name);
            ESP_LOGI(TAG, "Link key type: %d", param->auth_cmpl.lk_type);
            
            // Update state if this was part of connection process
            if (g_bt_state.state == BT_STATE_CONNECTING) {
                ESP_LOGI(TAG, "Pairing successful during connection");
                // A2DP connection should proceed automatically after successful pairing
            }
        } else {
            ESP_LOGE(TAG, "❌ Authentication FAILED with %s, status: %d", 
                     bt_addr_to_string(param->auth_cmpl.bda), param->auth_cmpl.stat);
            
            // If we were trying to connect, mark as failed
            if (g_bt_state.state == BT_STATE_CONNECTING) {
                g_bt_state.state = BT_STATE_IDLE;
                xEventGroupSetBits(bt_event_group, BT_CONNECT_COMPLETE_BIT);
            }
        }
        break;
    
    case ESP_BT_GAP_MODE_CHG_EVT:
        // GAP mode change event (power save mode changes)
        ESP_LOGD(TAG, "GAP mode changed: mode %d", param->mode_chg.mode);
        break;
    
    default:
        ESP_LOGD(TAG, "GAP event: %d", event);
        break;
    }
}

// A2DP callback for audio events
void bt_a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const char* addr_str = bt_addr_to_string(param->conn_stat.remote_bda);
        
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP connected to %s", addr_str);
            g_bt_state.a2dp_connected = true;
            g_bt_state.state = BT_STATE_CONNECTED;
            
            // Store connected device info (important for auto-connections during scan)
            memcpy(g_bt_state.connected_device_addr, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            
            // Try to find device name from discovered devices
            bool found_name = false;
            for (int i = 0; i < g_bt_state.discovered_device_count; i++) {
                if (memcmp(g_bt_state.discovered_devices[i].addr, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN) == 0) {
                    strncpy(g_bt_state.connected_device_name, 
                           g_bt_state.discovered_devices[i].name, 
                           sizeof(g_bt_state.connected_device_name) - 1);
                    g_bt_state.connected_device_name[sizeof(g_bt_state.connected_device_name) - 1] = '\0';
                    found_name = true;
                    ESP_LOGI(TAG, "Auto-connection detected during scan: %s", g_bt_state.connected_device_name);
                    break;
                }
            }
            
            if (!found_name) {
                // Fallback if device wasn't in discovered list
                snprintf(g_bt_state.connected_device_name, sizeof(g_bt_state.connected_device_name), 
                        "Device_%s", addr_str);
                ESP_LOGI(TAG, "Auto-connection detected to unknown device: %s", addr_str);
            }
            
            // Signal connection complete
            xEventGroupSetBits(bt_event_group, BT_CONNECT_COMPLETE_BIT);
            
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            ESP_LOGI(TAG, "A2DP connecting to %s", addr_str);
            g_bt_state.state = BT_STATE_CONNECTING;
            
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP disconnected from %s", addr_str);
            g_bt_state.a2dp_connected = false;
            g_bt_state.state = BT_STATE_IDLE;
            
            // Clear connected device info
            memset(g_bt_state.connected_device_addr, 0, ESP_BD_ADDR_LEN);
            memset(g_bt_state.connected_device_name, 0, sizeof(g_bt_state.connected_device_name));
            
            // Signal connection complete (even for disconnect)
            xEventGroupSetBits(bt_event_group, BT_CONNECT_COMPLETE_BIT);
            
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTING) {
            ESP_LOGI(TAG, "A2DP disconnecting from %s", addr_str);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state: %s", 
                 param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED ? "Started" : "Stopped");
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG, "A2DP audio config received - codec ready for streaming");
        break;
    default:
        ESP_LOGD(TAG, "A2DP event: %d", event);
        break;
    }
}

// AVRC callback for remote control events  
void bt_avrc_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRC connection state: %d", param->conn_stat.connected);
        break;
    default:
        ESP_LOGD(TAG, "AVRC event: %d", event);
        break;
    }
}

// Helper function to convert address string to esp_bd_addr_t
static int str_to_bda(const char* addr_str, esp_bd_addr_t bda) {
    if (!addr_str || !bda) return -1;
    
    // Parse "XX:XX:XX:XX:XX:XX" format
    int values[6];
    if (sscanf(addr_str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &values[0], &values[1], &values[2], 
               &values[3], &values[4], &values[5]) != 6) {
        ESP_LOGE(TAG, "Invalid MAC address format: %s", addr_str);
        return -1;
    }
    
    for (int i = 0; i < 6; i++) {
        bda[i] = (uint8_t)values[i];
    }
    
    ESP_LOGI(TAG, "Converted address %s to BDA", addr_str);
    return 0;
}

// Find device in discovered list by address
static int32_t find_device_by_address(const char* device_address) {
    if (!device_address) return -1;
    
    esp_bd_addr_t target_addr;
    if (str_to_bda(device_address, target_addr) != 0) {
        return -1;
    }
    
    for (int i = 0; i < g_bt_state.discovered_device_count; i++) {
        if (memcmp(g_bt_state.discovered_devices[i].addr, target_addr, ESP_BD_ADDR_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

// A2DP streaming buffer management using PSRAM
static int32_t a2dp_init_streaming_buffer(void) {
    if (a2dp_stream_buffer != NULL) {
        return 0; // Already allocated
    }
    
    ESP_LOGI(TAG, "Allocating A2DP streaming buffer in PSRAM (%zu bytes)", A2DP_BUFFER_SIZE);
    a2dp_stream_buffer = (int16_t*)mcp_malloc(A2DP_BUFFER_SIZE);
    if (!a2dp_stream_buffer) {
        ESP_LOGE(TAG, "Failed to allocate A2DP streaming buffer in PSRAM");
        return -1;
    }
    
    a2dp_buffer_size = A2DP_BUFFER_SIZE;
    a2dp_stream_time = 0.0f;
    
    ESP_LOGI(TAG, "A2DP streaming buffer allocated successfully in PSRAM");
    return 0;
}

static void a2dp_cleanup_streaming_buffer(void) {
    if (a2dp_stream_buffer) {
        ESP_LOGI(TAG, "Freeing A2DP streaming buffer from PSRAM");
        mcp_free(a2dp_stream_buffer);
        a2dp_stream_buffer = NULL;
        a2dp_buffer_size = 0;
    }
    a2dp_streaming_active = false;
    a2dp_stream_time = 0.0f;
}

// A2DP audio data callback - generates audio samples for streaming
static int32_t a2dp_audio_callback(uint8_t* data, int32_t len) {
    static int callback_count = 0;
    callback_count++;
    
    // Log every 100th callback to avoid log spam, plus first few
    if (callback_count <= 5 || callback_count % 100 == 0) {
        ESP_LOGI(TAG, "A2DP callback #%d: data=%p, len=%d", callback_count, data, len);
    }
    
    // Add NULL pointer checks
    if (!data) {
        ESP_LOGE(TAG, "A2DP callback: NULL data pointer!");
        return 0;
    }
    
    if (len <= 0) {
        ESP_LOGW(TAG, "A2DP callback: Invalid length %d", len);
        return 0;
    }
    
    if (!a2dp_stream_buffer || !a2dp_streaming_active) {
        // No audio to stream, send silence
        if (callback_count <= 3) {
            ESP_LOGI(TAG, "A2DP callback: Sending silence (buffer=%p, active=%d)", 
                     a2dp_stream_buffer, a2dp_streaming_active);
        }
        memset(data, 0, len);
        return len;
    }
    
    int16_t* samples = (int16_t*)data;
    int32_t sample_count = len / sizeof(int16_t);
    
    if (callback_count <= 3) {
        ESP_LOGI(TAG, "A2DP callback: samples=%p, sample_count=%d", samples, sample_count);
    }
    
    // Check if we're playing a melody
    if (!a2dp_melody_playing || !a2dp_melody_notes || a2dp_melody_count == 0) {
        // No melody playing, send silence
        if (callback_count <= 3) {
            ESP_LOGI(TAG, "A2DP callback: No melody playing (playing=%d, notes=%p, count=%d)", 
                     a2dp_melody_playing, a2dp_melody_notes, a2dp_melody_count);
        }
        memset(data, 0, len);
        return len;
    }
    
    // Get current time and check if we need to advance to next note
    int64_t current_time_us = esp_timer_get_time();
    int64_t elapsed_ms = (current_time_us - a2dp_note_start_time_us) / 1000;
    
    if (callback_count <= 3) {
        ESP_LOGI(TAG, "A2DP callback: current_note_index=%d, elapsed_ms=%lld", 
                 a2dp_current_note_index, elapsed_ms);
    }
    
    // Check if current note duration has elapsed
    if (a2dp_current_note_index < a2dp_melody_count) {
        a2dp_note_t* current_note = &a2dp_melody_notes[a2dp_current_note_index];
        
        if (elapsed_ms >= current_note->duration_ms) {
            // Move to next note
            a2dp_current_note_index++;
            a2dp_note_start_time_us = current_time_us;
            
            if (a2dp_current_note_index < a2dp_melody_count) {
                ESP_LOGI(TAG, "A2DP: Advanced to note %d (frequency=%.1fHz)", 
                         a2dp_current_note_index, 
                         a2dp_melody_notes[a2dp_current_note_index].frequency);
            } else {
                ESP_LOGI(TAG, "A2DP: Advanced past final note (%d >= %d)", 
                         a2dp_current_note_index, a2dp_melody_count);
            }
            
            if (a2dp_current_note_index >= a2dp_melody_count) {
                // Melody finished - just stop playing, no SUSPEND to avoid crashes
                a2dp_melody_playing = false;
                memset(data, 0, len);
                ESP_LOGI(TAG, "Melody playback completed - stream continues with silence");
                return len;
            }
        }
    }
    
    // Generate audio for current note
    if (a2dp_current_note_index < a2dp_melody_count) {
        float frequency = a2dp_melody_notes[a2dp_current_note_index].frequency;
        float volume_scale = (float)a2dp_melody_volume / 100.0f;
        
        if (callback_count <= 3) {
            ESP_LOGI(TAG, "A2DP callback: Generating audio - freq=%.1fHz, vol=%.2f", 
                     frequency, volume_scale);
        }
        
        // Add bounds check for sample array access
        if (sample_count > 2000) {  // Sanity check - typical is 128-512 samples
            ESP_LOGE(TAG, "A2DP callback: Suspiciously large sample_count=%d", sample_count);
            memset(data, 0, len);
            return len;
        }
        
        // Calculate phase increment for this frequency
        float phase_increment = frequency / (float)A2DP_SAMPLE_RATE;
        
        for (int32_t i = 0; i < sample_count; i += 2) {
            // Safety check before array access
            if (i + 1 >= sample_count) {
                ESP_LOGE(TAG, "A2DP callback: Array bounds error i=%d, sample_count=%d", i, sample_count);
                break;
            }
            
            // Generate stereo sine wave using lookup table
            int16_t raw_sample = a2dp_sine_lookup(a2dp_phase);
            int16_t sample = (int16_t)((float)raw_sample * volume_scale * 0.5f); // Scale down to prevent clipping
            
            samples[i] = sample;     // Left channel
            samples[i + 1] = sample; // Right channel
            
            // Advance phase with wrap-around for continuity
            a2dp_phase += phase_increment;
            if (a2dp_phase >= 1.0f) {
                a2dp_phase -= 1.0f;
            }
        }
        
        if (callback_count <= 3) {
            ESP_LOGI(TAG, "A2DP callback: Generated %d samples successfully", sample_count);
        }
    } else {
        // No more notes, send silence
        if (callback_count <= 3) {
            ESP_LOGI(TAG, "A2DP callback: No current note, sending silence");
        }
        memset(data, 0, len);
    }
    
    return len;
}

// Sine Lookup Table Functions

// Initialize sine lookup table in PSRAM for efficient audio generation
static int32_t a2dp_init_sine_table(void) {
    if (sine_lookup_table != NULL) {
        return 0; // Already initialized
    }
    
    ESP_LOGI(TAG, "Initializing sine lookup table (%d samples)", SINE_TABLE_SIZE);
    sine_lookup_table = (int16_t*)mcp_malloc(SINE_TABLE_SIZE * sizeof(int16_t));
    if (!sine_lookup_table) {
        ESP_LOGE(TAG, "Failed to allocate sine lookup table in PSRAM");
        return -1;
    }
    
    // Pre-calculate sine values (16-bit signed, full amplitude range)
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        float angle = 2.0f * M_PI * (float)i / (float)SINE_TABLE_SIZE;
        sine_lookup_table[i] = (int16_t)(32767.0f * sinf(angle));
    }
    
    ESP_LOGI(TAG, "Sine lookup table initialized successfully");
    return 0;
}

// Clean up sine lookup table
static void a2dp_cleanup_sine_table(void) {
    if (sine_lookup_table) {
        ESP_LOGI(TAG, "Freeing sine lookup table");
        mcp_free(sine_lookup_table);
        sine_lookup_table = NULL;
    }
}

// Fast sine lookup with linear interpolation for smooth audio
static inline int16_t a2dp_sine_lookup(float phase) {
    if (!sine_lookup_table) return 0;
    
    // Wrap phase to 0.0-1.0 range
    phase = phase - floorf(phase);
    if (phase < 0.0f) phase += 1.0f;
    
    // Convert to table index with fractional part
    float table_pos = phase * (float)SINE_TABLE_SIZE;
    int index = (int)table_pos;
    float frac = table_pos - (float)index;
    
    // Get two adjacent samples
    int16_t sample1 = sine_lookup_table[index & (SINE_TABLE_SIZE - 1)];
    int16_t sample2 = sine_lookup_table[(index + 1) & (SINE_TABLE_SIZE - 1)];
    
    // Linear interpolation for smoother audio
    return (int16_t)(sample1 + frac * (sample2 - sample1));
}

// A2DP Melody Helper Functions

// Convert note name to frequency (simplified version)
static float a2dp_note_to_frequency(const char* note) {
    // Simplified note lookup table - just the basics for now
    if (strncmp(note, "C4", 2) == 0) return 261.63f;
    if (strncmp(note, "D4", 2) == 0) return 293.66f;
    if (strncmp(note, "E4", 2) == 0) return 329.63f;
    if (strncmp(note, "F4", 2) == 0) return 349.23f;
    if (strncmp(note, "G4", 2) == 0) return 392.00f;
    if (strncmp(note, "A4", 2) == 0) return 440.00f;
    if (strncmp(note, "B4", 2) == 0) return 493.88f;
    
    // Default to A4 if note not recognized
    ESP_LOGW(TAG, "Unknown note: %s, using A4 (440Hz)", note);
    return 440.0f;
}

// Parse melody string into note array (now expects frequencies, not note names)
static int32_t a2dp_parse_melody(const char* melody_str) {
    if (!melody_str) return -1;
    
    // Clean up any existing melody
    a2dp_cleanup_melody();
    
    // Count notes first
    int note_count = 1; // At least one note
    for (const char* p = melody_str; *p; p++) {
        if (*p == ',') note_count++;
    }
    
    // Allocate memory for notes
    a2dp_melody_notes = (a2dp_note_t*)mcp_malloc(note_count * sizeof(a2dp_note_t));
    if (!a2dp_melody_notes) {
        ESP_LOGE(TAG, "Failed to allocate memory for melody notes");
        return -1;
    }
    
    // Parse notes
    char melody_copy[512];
    strncpy(melody_copy, melody_str, sizeof(melody_copy) - 1);
    melody_copy[sizeof(melody_copy) - 1] = '\0';
    
    char* token = strtok(melody_copy, ",");
    int parsed_count = 0;
    
    while (token && parsed_count < note_count) {
        // Parse frequency:duration format (e.g., "329.63:500")
        char* colon = strchr(token, ':');
        int duration = 500; // Default duration
        float frequency = 440.0f; // Default frequency (A4)
        
        if (colon) {
            *colon = '\0';
            duration = atoi(colon + 1);
            if (duration <= 0) duration = 500;
        }
        
        // Parse frequency directly (no longer converting from note names)
        frequency = atof(token);
        if (frequency <= 0.0f) {
            ESP_LOGW(TAG, "Invalid frequency: %s, using 440Hz", token);
            frequency = 440.0f;
        }
        
        a2dp_melody_notes[parsed_count].frequency = frequency;
        a2dp_melody_notes[parsed_count].duration_ms = duration;
        
        ESP_LOGD(TAG, "Parsed note %d: %.2fHz, %dms", parsed_count, frequency, duration);
        
        parsed_count++;
        token = strtok(NULL, ",");
    }
    
    a2dp_melody_count = parsed_count;
    ESP_LOGI(TAG, "Parsed %d notes from melody", a2dp_melody_count);
    
    return 0;
}

// Clean up melody resources
static void a2dp_cleanup_melody(void) {
    if (a2dp_melody_notes) {
        mcp_free(a2dp_melody_notes);
        a2dp_melody_notes = NULL;
    }
    a2dp_melody_count = 0;
    a2dp_current_note_index = 0;
    a2dp_melody_playing = false;
}

// Start melody playback
static void a2dp_start_melody(void) {
    if (a2dp_melody_count > 0) {
        a2dp_current_note_index = 0;
        a2dp_note_start_time_us = esp_timer_get_time();
        a2dp_melody_playing = true;
        ESP_LOGI(TAG, "Started melody with %d notes", a2dp_melody_count);
    }
}

// Stop melody playback
static void a2dp_stop_melody(void) {
    a2dp_melody_playing = false;
    ESP_LOGI(TAG, "Stopped melody playback");
}

// Start A2DP streaming
int32_t bluetooth_start_a2dp_streaming(wasm_exec_env_t exec_env) {
    ESP_LOGI(TAG, "Starting A2DP streaming");
    
    if (!g_bt_state.initialized || !g_bt_state.a2dp_connected) {
        ESP_LOGE(TAG, "Bluetooth not connected for A2DP streaming");
        return -1;
    }
    
    // Initialize PSRAM buffer
    if (a2dp_init_streaming_buffer() != 0) {
        return -2;
    }
    
    // Initialize sine lookup table
    if (a2dp_init_sine_table() != 0) {
        return -3;
    }
    
    // Reset phase for clean start
    a2dp_phase = 0.0f;
    
    // Register A2DP source data callback
    esp_err_t ret = esp_a2d_source_register_data_callback(a2dp_audio_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register A2DP data callback: %s", esp_err_to_name(ret));
        return -4;
    }
    
    // Start media transmission - this triggers the audio callback
    ret = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start A2DP media transmission: %s", esp_err_to_name(ret));
        return -5;
    }
    
    a2dp_streaming_active = true;
    ESP_LOGI(TAG, "A2DP streaming started successfully");
    return 0;
}

// Stop A2DP streaming
int32_t bluetooth_stop_a2dp_streaming(wasm_exec_env_t exec_env) {
    ESP_LOGI(TAG, "Stopping A2DP streaming");
    
    a2dp_streaming_active = false;
    
    // Unregister callback
    esp_a2d_source_register_data_callback(NULL);
    
    // Keep buffer allocated for potential reuse
    ESP_LOGI(TAG, "A2DP streaming stopped");
    return 0;
}

// Placeholder implementations for functions we'll implement later
int32_t bluetooth_pair_device(wasm_exec_env_t exec_env, const char* device_name) {
    ESP_LOGI(TAG, "Pair device: %s (not yet implemented)", device_name);
    return -1; // Not implemented yet
}

int32_t bluetooth_connect_device(wasm_exec_env_t exec_env, const char* device_address) {
    ESP_LOGI(TAG, "Connecting to device: %s", device_address);
    
    if (!g_bt_state.initialized) {
        ESP_LOGE(TAG, "Bluetooth not initialized");
        return -1;
    }
    
    if (!device_address) {
        ESP_LOGE(TAG, "Invalid device address");
        return -2;
    }
    
    // Convert address string to esp_bd_addr_t first
    esp_bd_addr_t target_addr;
    if (str_to_bda(device_address, target_addr) != 0) {
        ESP_LOGE(TAG, "Failed to parse device address");
        return -5;
    }
    
    // Check if already connected to this specific device
    if (g_bt_state.state == BT_STATE_CONNECTED) {
        if (memcmp(g_bt_state.connected_device_addr, target_addr, ESP_BD_ADDR_LEN) == 0) {
            ESP_LOGI(TAG, "Already connected to requested device %s - returning success", device_address);
            return 0; // Success - already connected to the requested device
        }
        
        ESP_LOGW(TAG, "Already connected to a different device, disconnecting first");
        bluetooth_disconnect(exec_env);
        // Wait a bit for disconnection
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Verify device was discovered and is audio-capable
    int32_t device_index = find_device_by_address(device_address);
    if (device_index < 0) {
        ESP_LOGE(TAG, "Device %s not found in scan results. Run bluetooth_scan first.", device_address);
        return -3;
    }
    
    bt_discovered_device_t* device = &g_bt_state.discovered_devices[device_index];
    if (!device->is_audio_device) {
        ESP_LOGE(TAG, "Device %s is not an audio device", device_address);
        return -4;
    }
    
    ESP_LOGI(TAG, "Connecting to audio device: %s (%s)", 
             device->name[0] ? device->name : "Unknown", device_address);
    
    // Clear connection complete bit
    xEventGroupClearBits(bt_event_group, BT_CONNECT_COMPLETE_BIT);
    
    // Set connecting state
    g_bt_state.state = BT_STATE_CONNECTING;
    memcpy(g_bt_state.connected_device_addr, target_addr, ESP_BD_ADDR_LEN);
    strncpy(g_bt_state.connected_device_name, device->name, sizeof(g_bt_state.connected_device_name) - 1);
    g_bt_state.connected_device_name[sizeof(g_bt_state.connected_device_name) - 1] = '\0';
    
    ESP_LOGI(TAG, "Initiating A2DP source connection to %s", bt_addr_to_string(target_addr));
    
    // Initiate A2DP source connection (ESP32 streams audio TO speaker)
    esp_err_t ret = esp_a2d_source_connect(target_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start A2DP connection: %s", esp_err_to_name(ret));
        g_bt_state.state = BT_STATE_IDLE;
        return -6;
    }
    
    ESP_LOGI(TAG, "A2DP connection initiated, waiting for completion...");
    
    // Wait for connection to complete (with timeout)
    EventBits_t bits = xEventGroupWaitBits(bt_event_group, BT_CONNECT_COMPLETE_BIT,
                                          pdTRUE, pdFALSE, pdMS_TO_TICKS(15000)); // 15 second timeout
    
    if (bits & BT_CONNECT_COMPLETE_BIT) {
        if (g_bt_state.state == BT_STATE_CONNECTED) {
            ESP_LOGI(TAG, "Successfully connected to %s", device->name);
            return 0;
        } else {
            ESP_LOGE(TAG, "Connection completed but not in connected state");
            return -7;
        }
    } else {
        ESP_LOGE(TAG, "Connection timeout after 15 seconds");
        g_bt_state.state = BT_STATE_IDLE;
        return -8;
    }
}

int32_t bluetooth_disconnect(wasm_exec_env_t exec_env) {
    ESP_LOGI(TAG, "Disconnect (not yet implemented)");
    return -1; // Not implemented yet
}

int32_t bluetooth_get_connected_device_address(wasm_exec_env_t exec_env, char* buffer, int32_t max_len) {
    ESP_LOGI(TAG, "Get connected device address (not yet implemented)");
    return -1; // Not implemented yet
}

int32_t bluetooth_play_tone(wasm_exec_env_t exec_env, int32_t frequency_hz, int32_t duration_ms, int32_t volume_percent) {
    ESP_LOGI(TAG, "Play tone via Bluetooth (not yet implemented)");
    return -1; // Not implemented yet
}

int32_t bluetooth_play_melody(wasm_exec_env_t exec_env, const char* melody_str, int32_t volume_percent) {
    ESP_LOGI(TAG, "Starting Bluetooth A2DP melody playback");
    
    if (!g_bt_state.a2dp_connected) {
        ESP_LOGE(TAG, "A2DP not connected - cannot play melody");
        return -1;
    }
    
    // Parse the melody string into notes
    if (a2dp_parse_melody(melody_str) != 0) {
        ESP_LOGE(TAG, "Failed to parse melody string");
        return -2;
    }
    
    // Set volume (clamp to valid range)
    a2dp_melody_volume = (volume_percent < 0) ? 0 : (volume_percent > 100) ? 100 : volume_percent;
    
    // Initialize A2DP streaming buffer if needed
    if (a2dp_init_streaming_buffer() != 0) {
        ESP_LOGE(TAG, "Failed to initialize A2DP streaming buffer");
        a2dp_cleanup_melody();
        return -3;
    }
    
    // Start melody playback
    a2dp_start_melody();
    
    // Start A2DP streaming (this will trigger the audio callback)
    if (bluetooth_start_a2dp_streaming(exec_env) != 0) {
        ESP_LOGE(TAG, "Failed to start A2DP streaming");
        a2dp_stop_melody();
        return -4;
    }
    
    ESP_LOGI(TAG, "A2DP melody playback started successfully");
    return 0;
}

int32_t bluetooth_set_volume(wasm_exec_env_t exec_env, int32_t volume_percent) {
    ESP_LOGI(TAG, "Set Bluetooth volume to %d%% (not yet implemented)", volume_percent);
    return -1; // Not implemented yet
}