idf_component_register(SRCS "main.c" 
                            "../mcp_esp32_framework/host/src/mcp_config_loader.c" 
                            "../mcp_esp32_framework/host/src/mcp_host_functions.c"
                            "../mcp_esp32_framework/host/src/mcp_bluetooth_host.c"
                            "../mcp_esp32_framework/host/src/mcp_http_server.c"
                            "../mcp_esp32_framework/host/src/mcp_wifi_minimal.c"
                            "../mcp_esp32_framework/host/src/mcp_wasm_runtime.c"
                            "../mcp_esp32_framework/host/src/mcp_pthread_pool.c"
                            "../mcp_esp32_framework/host/src/mcp_session_manager.c"
                            "../mcp_esp32_framework/host/src/mcp_memory.c"
                    INCLUDE_DIRS "." "../mcp_esp32_framework/host/include"
                    REQUIRES driver wamr fatfs esp_http_server nvs_flash esp_wifi bt)

# WiFi credentials are read from <project>/.env (gitignored) - see .env.example
idf_build_get_property(project_dir PROJECT_DIR)
set(env_file "${project_dir}/.env")
if(NOT EXISTS "${env_file}")
    message(FATAL_ERROR "Missing ${env_file} - copy .env.example to .env and set WIFI_SSID / WIFI_PASS")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${env_file}")
file(STRINGS "${env_file}" env_lines REGEX "^(WIFI_SSID|WIFI_PASS)=")
foreach(line IN LISTS env_lines)
    string(REGEX MATCH "^([A-Z_]+)=(.*)$" _ "${line}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "${CMAKE_MATCH_1}=\"${CMAKE_MATCH_2}\"")
endforeach()
