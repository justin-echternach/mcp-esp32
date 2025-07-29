idf_component_register(SRCS "main.c" "mcp_pthread_handler.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver wamr fatfs esp_http_server nvs_flash esp_wifi)