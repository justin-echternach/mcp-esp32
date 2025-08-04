#include "mcp_wifi_minimal.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"

// EXACT COPY from original working main.c - DO NOT CHANGE ANYTHING
void mcp_wifi_init_sta(wifi_event_handler_t event_handler) {
    g_wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    
    // EXACT wifi_config initialization from original - compile time constants
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "your-ssid",
            .password = "your-password",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(g_wifi_tag, "WiFi initialization finished");
    
    // EXACT wait logic from original
    EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group,
            g_wifi_connected_bit | BIT1,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
    
    if (bits & g_wifi_connected_bit) {
        ESP_LOGI(g_wifi_tag, "Connected to WiFi SSID: your-ssid");
    } else {
        ESP_LOGE(g_wifi_tag, "Failed to connect to WiFi");
    }
}