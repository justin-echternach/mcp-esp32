idf_component_register(SRCS "main.c" 
                            "../mcp_framework/src/mcp_config_loader.c" 
                            "../mcp_framework/src/mcp_host_functions.c"
                            "../mcp_framework/src/mcp_http_server.c"
                            "../mcp_framework/src/mcp_wifi_minimal.c"
                            "../mcp_framework/src/mcp_wasm_runtime.c"
                            "../mcp_framework/src/mcp_pthread_pool.c"
                    INCLUDE_DIRS "." "../mcp_framework/include"
                    REQUIRES driver wamr fatfs esp_http_server nvs_flash esp_wifi)