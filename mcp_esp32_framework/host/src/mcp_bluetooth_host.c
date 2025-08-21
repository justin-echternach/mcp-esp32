#include "mcp_bluetooth_host.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>


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
    
    // Initialize A2DP sink
    ret = esp_a2d_register_callback(bt_a2dp_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP register callback failed: %s", esp_err_to_name(ret));
        return -7;
    }
    
    ret = esp_a2d_sink_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP sink init failed: %s", esp_err_to_name(ret));
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
    
    // Deinitialize components in reverse order
    esp_avrc_ct_deinit();
    esp_a2d_sink_deinit();
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
                ESP_LOGI(TAG, "CoD: 0x%06X, Svc:0x%03X, Major:%d, Valid:%s, Phone:%s, AV:%s, AudioSvc:%s, Final:%s",
                         cod, service_class, major_dev,
                         is_valid_cod ? "Y" : "N",
                         is_phone_device ? "Y" : "N", 
                         is_av_device ? "Y" : "N",
                         has_audio_service ? "Y" : "N",
                         is_audio_device ? "YES" : "NO");
                break;
            }
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *)(prop->val);
                break;
            default:
                break;
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
    default:
        ESP_LOGD(TAG, "GAP event: %d", event);
        break;
    }
}

// A2DP callback for audio events
void bt_a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP connected");
            g_bt_state.a2dp_connected = true;
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP disconnected");
            g_bt_state.a2dp_connected = false;
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state: %s", 
                 param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED ? "Started" : "Stopped");
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

// Placeholder implementations for functions we'll implement later
int32_t bluetooth_pair_device(wasm_exec_env_t exec_env, const char* device_name) {
    ESP_LOGI(TAG, "Pair device: %s (not yet implemented)", device_name);
    return -1; // Not implemented yet
}

int32_t bluetooth_connect_device(wasm_exec_env_t exec_env, const char* device_address) {
    ESP_LOGI(TAG, "Connect device: %s (not yet implemented)", device_address);
    return -1; // Not implemented yet
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
    ESP_LOGI(TAG, "Play melody via Bluetooth (not yet implemented)");
    return -1; // Not implemented yet  
}

int32_t bluetooth_set_volume(wasm_exec_env_t exec_env, int32_t volume_percent) {
    ESP_LOGI(TAG, "Set Bluetooth volume to %d%% (not yet implemented)", volume_percent);
    return -1; // Not implemented yet
}