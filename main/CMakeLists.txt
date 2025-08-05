idf_component_register(SRCS "main.c" 
                            "../mcp_esp32_framework/host/src/mcp_config_loader.c" 
                            "../mcp_esp32_framework/host/src/mcp_host_functions.c"
                            "../mcp_esp32_framework/host/src/mcp_http_server.c"
                            "../mcp_esp32_framework/host/src/mcp_wifi_minimal.c"
                            "../mcp_esp32_framework/host/src/mcp_wasm_runtime.c"
                            "../mcp_esp32_framework/host/src/mcp_pthread_pool.c"
                    INCLUDE_DIRS "." "../mcp_esp32_framework/host/include"
                    REQUIRES driver wamr fatfs esp_http_server nvs_flash esp_wifi)