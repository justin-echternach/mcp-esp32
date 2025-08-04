#ifndef MCP_WIFI_MINIMAL_H
#define MCP_WIFI_MINIMAL_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

// Minimal WiFi framework - exact copy of working functions
// But use external globals that main.c will provide
extern EventGroupHandle_t g_wifi_event_group;
extern int g_s_retry_num;
extern const int g_wifi_connected_bit;
extern int g_wifi_maximum_retry;
extern const char* g_wifi_tag;

// Event handler type
typedef void (*wifi_event_handler_t)(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

// Exact function signatures from main.c (but event handler stays in main.c)
void mcp_wifi_init_sta(const char* ssid, const char* password, wifi_event_handler_t event_handler);

#ifdef __cplusplus
}
#endif

#endif // MCP_WIFI_MINIMAL_H