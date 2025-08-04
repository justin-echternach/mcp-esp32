idf_component_register(SRCS "main.c" "mcp_pthread_handler.c" "../mcp_framework/src/mcp_config_loader.c" "../mcp_framework/src/mcp_host_functions.c"
                    INCLUDE_DIRS "." "../mcp_framework/include"
                    REQUIRES driver wamr fatfs esp_http_server nvs_flash esp_wifi)