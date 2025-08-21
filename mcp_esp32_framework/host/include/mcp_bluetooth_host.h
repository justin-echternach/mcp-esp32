#ifndef MCP_BLUETOOTH_HOST_H
#define MCP_BLUETOOTH_HOST_H

#include <stdint.h>
#include <stdbool.h>

// ESP-IDF Bluetooth includes
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "wasm_export.h"

// Maximum devices we can discover and store
#define MAX_DISCOVERED_DEVICES 10
#define MAX_DEVICE_NAME_LEN 64
#define MAX_SCAN_RESULTS_JSON 2048

// Bluetooth connection states
typedef enum {
    BT_STATE_IDLE,
    BT_STATE_SCANNING,
    BT_STATE_PAIRING,
    BT_STATE_CONNECTING,
    BT_STATE_CONNECTED
} bt_connection_state_t;

// Discovered device info
typedef struct {
    esp_bd_addr_t addr;
    char name[MAX_DEVICE_NAME_LEN];
    int32_t rssi;
    bool is_audio_device;
} bt_discovered_device_t;

// Global Bluetooth state
typedef struct {
    bool initialized;
    bt_connection_state_t state;
    esp_bd_addr_t connected_device_addr;
    char connected_device_name[MAX_DEVICE_NAME_LEN];
    bt_discovered_device_t discovered_devices[MAX_DISCOVERED_DEVICES];
    int discovered_device_count;
    bool a2dp_connected;
    int current_volume;
} bt_state_t;

// Host function declarations - these will be exposed to WASM
int32_t bluetooth_init(wasm_exec_env_t exec_env);
int32_t bluetooth_deinit(wasm_exec_env_t exec_env);
int32_t bluetooth_scan_start(wasm_exec_env_t exec_env, int32_t timeout_sec);
int32_t bluetooth_scan_get_results(wasm_exec_env_t exec_env, char* buffer, int32_t max_len);
int32_t bluetooth_pair_device(wasm_exec_env_t exec_env, const char* device_name);
int32_t bluetooth_connect_device(wasm_exec_env_t exec_env, const char* device_address);
int32_t bluetooth_disconnect(wasm_exec_env_t exec_env);
int32_t bluetooth_is_connected(wasm_exec_env_t exec_env);
int32_t bluetooth_get_connected_device_name(wasm_exec_env_t exec_env, char* buffer, int32_t max_len);
int32_t bluetooth_get_connected_device_address(wasm_exec_env_t exec_env, char* buffer, int32_t max_len);
int32_t bluetooth_play_tone(wasm_exec_env_t exec_env, int32_t frequency_hz, int32_t duration_ms, int32_t volume_percent);
int32_t bluetooth_play_melody(wasm_exec_env_t exec_env, const char* melody_str, int32_t volume_percent);
int32_t bluetooth_set_volume(wasm_exec_env_t exec_env, int32_t volume_percent);

// Internal helper functions
void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
void bt_a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
void bt_avrc_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

// Utility functions
const char* bt_addr_to_string(esp_bd_addr_t addr);
bool bt_string_to_addr(const char* str, esp_bd_addr_t addr);
int32_t bt_find_device_by_name(const char* device_name);

#endif // MCP_BLUETOOTH_HOST_H