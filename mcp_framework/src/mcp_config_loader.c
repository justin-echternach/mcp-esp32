#include "mcp_config_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "esp_log.h"

static const char* TAG = "MCP_CONFIG";

// ========== JSON Parsing Utilities ==========

bool mcp_json_find_string(const char* json, const char* key, char* value, size_t max_len) {
    if (!json || !key || !value || max_len == 0) {
        return false;
    }
    
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    const char* key_pos = strstr(json, search_key);
    if (!key_pos) return false;
    
    // Find the opening quote of the value
    const char* value_start = strchr(key_pos + strlen(search_key), '"');
    if (!value_start) return false;
    value_start++; // Skip opening quote
    
    // Find the closing quote
    const char* value_end = strchr(value_start, '"');
    if (!value_end) return false;
    
    size_t len = value_end - value_start;
    if (len >= max_len) len = max_len - 1;
    
    strncpy(value, value_start, len);
    value[len] = '\0';
    return true;
}

bool mcp_json_find_int(const char* json, const char* key, int* value) {
    if (!json || !key || !value) {
        return false;
    }
    
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    const char* key_pos = strstr(json, search_key);
    if (!key_pos) return false;
    
    // Skip to the value part
    const char* value_start = key_pos + strlen(search_key);
    while (*value_start == ' ' || *value_start == '\t') value_start++;
    
    *value = atoi(value_start);
    return true;
}

bool mcp_json_find_bool(const char* json, const char* key, bool* value) {
    if (!json || !key || !value) {
        return false;
    }
    
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    const char* key_pos = strstr(json, search_key);
    if (!key_pos) return false;
    
    const char* value_start = key_pos + strlen(search_key);
    while (*value_start == ' ' || *value_start == '\t') value_start++;
    
    if (strncmp(value_start, "true", 4) == 0) {
        *value = true;
        return true;
    } else if (strncmp(value_start, "false", 5) == 0) {
        *value = false;
        return true;
    }
    return false;
}

int mcp_json_parse_int_array(const char* json, const char* key, int* array, int max_count) {
    if (!json || !key || !array || max_count <= 0) {
        return 0;
    }
    
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":[", key);
    
    const char* array_start = strstr(json, search_key);
    if (!array_start) return 0;
    
    array_start += strlen(search_key);
    const char* array_end = strchr(array_start, ']');
    if (!array_end) return 0;
    
    int count = 0;
    const char* pos = array_start;
    
    while (pos < array_end && count < max_count) {
        while (*pos == ' ' || *pos == '\t' || *pos == ',') pos++;
        if (pos >= array_end) break;
        
        array[count] = atoi(pos);
        count++;
        
        // Find next comma or end
        while (pos < array_end && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }
    
    return count;
}

// ========== Configuration Loading ==========

bool mcp_config_init_defaults(mcp_config_t* config) {
    if (!config) {
        return false;
    }
    
    // Clear the entire structure
    memset(config, 0, sizeof(mcp_config_t));
    
    // Set system defaults
    config->system.max_wasm_modules = 2;
    config->system.wasm_heap_size = 8192;      // 8KB heap
    config->system.wasm_stack_size = 4096;     // 4KB stack  
    config->system.sse_max_clients = 8;
    config->system.mcp_timeout_ms = 5000;
    config->require_auth = false;
    config->rate_limit_per_minute = 60;
    
    // Set default protected pins (critical ESP32 pins)
    int default_protected[] = {0, 1, 6, 7, 8, 9, 10, 11};
    int default_protected_count = sizeof(default_protected) / sizeof(default_protected[0]);
    
    if (default_protected_count <= MAX_PROTECTED_PINS) {
        memcpy(config->gpio_protection.protected_pins, default_protected, sizeof(default_protected));
        config->gpio_protection.protected_pin_count = default_protected_count;
    }
    
    // Set default input-only pins (ADC pins)
    int default_input_only[] = {34, 35, 36, 37, 38, 39};
    int default_input_only_count = sizeof(default_input_only) / sizeof(default_input_only[0]);
    
    if (default_input_only_count <= MAX_PROTECTED_PINS) {
        memcpy(config->gpio_protection.input_only_pins, default_input_only, sizeof(default_input_only));
        config->gpio_protection.input_only_pin_count = default_input_only_count;
    }
    
    // Set default high-current pins
    int default_max_current[] = {12, 13, 14, 15};
    int default_max_current_count = sizeof(default_max_current) / sizeof(default_max_current[0]);
    
    if (default_max_current_count <= MAX_PROTECTED_PINS) {
        memcpy(config->gpio_protection.max_current_pins, default_max_current, sizeof(default_max_current));
        config->gpio_protection.max_current_pin_count = default_max_current_count;
    }
    
    ESP_LOGI(TAG, "MCP configuration initialized with defaults");
    return true;
}

bool mcp_config_load_from_json(mcp_config_t* config, const char* json_string) {
    if (!config || !json_string) {
        return false;
    }
    
    // Parse GPIO protection settings
    config->gpio_protection.protected_pin_count = mcp_json_parse_int_array(
        json_string, "protected_pins", 
        config->gpio_protection.protected_pins, 
        MAX_PROTECTED_PINS
    );
    
    config->gpio_protection.input_only_pin_count = mcp_json_parse_int_array(
        json_string, "input_only_pins",
        config->gpio_protection.input_only_pins,
        MAX_PROTECTED_PINS
    );
    
    config->gpio_protection.max_current_pin_count = mcp_json_parse_int_array(
        json_string, "max_current_pins",
        config->gpio_protection.max_current_pins,
        MAX_PROTECTED_PINS
    );
    
    // Parse system settings
    mcp_json_find_int(json_string, "max_wasm_modules", &config->system.max_wasm_modules);
    mcp_json_find_int(json_string, "wasm_heap_size", &config->system.wasm_heap_size);
    mcp_json_find_int(json_string, "wasm_stack_size", &config->system.wasm_stack_size);
    mcp_json_find_int(json_string, "sse_max_clients", &config->system.sse_max_clients);
    mcp_json_find_int(json_string, "mcp_timeout_ms", &config->system.mcp_timeout_ms);
    
    // Parse security settings
    mcp_json_find_bool(json_string, "require_auth", &config->require_auth);
    mcp_json_find_int(json_string, "rate_limit_per_minute", &config->rate_limit_per_minute);
    
    // Parse routes from nested JSON structure
    config->route_count = 0;
    
    // Find the "routes" object in the JSON
    const char* routes_start = strstr(json_string, "\"routes\"");
    if (routes_start) {
        // Find the opening brace of the routes object
        const char* routes_obj = strchr(routes_start, '{');
        if (routes_obj) {
            routes_obj++; // Skip the opening brace
            
            // Parse individual route entries
            const char* route_patterns[] = {"\"/mcp\"", "\"/mcp/led\"", "\"/mcp/gpio\"", "\"/mcp/system\""};
            const char* route_names[] = {"/mcp", "/mcp/led", "/mcp/gpio", "/mcp/system"};
            
            for (int i = 0; i < 4 && config->route_count < MAX_MCP_ROUTES; i++) {
                const char* route_entry = strstr(routes_obj, route_patterns[i]);
                if (route_entry) {
                    // Find the value after the colon
                    const char* colon = strchr(route_entry, ':');
                    if (colon) {
                        colon++; // Skip colon
                        while (*colon == ' ' || *colon == '\t') colon++; // Skip whitespace
                        
                        if (*colon == '"') {
                            colon++; // Skip opening quote
                            char wasm_file[MAX_FILENAME_LENGTH];
                            int j = 0;
                            while (*colon && *colon != '"' && j < MAX_FILENAME_LENGTH - 1) {
                                wasm_file[j++] = *colon++;
                            }
                            wasm_file[j] = '\0';
                            
                            // Store the route
                            strncpy(config->routes[config->route_count].route, route_names[i], MAX_ROUTE_LENGTH - 1);
                            strncpy(config->routes[config->route_count].wasm_file, wasm_file, MAX_FILENAME_LENGTH - 1);
                            config->route_count++;
                            
                            ESP_LOGI(TAG, "Parsed route: %s -> %s", route_names[i], wasm_file);
                        }
                    }
                }
            }
        }
    }
    
    ESP_LOGI(TAG, "MCP config loaded from JSON:");
    ESP_LOGI(TAG, "  Protected pins: %d", config->gpio_protection.protected_pin_count);
    ESP_LOGI(TAG, "  Input-only pins: %d", config->gpio_protection.input_only_pin_count);
    ESP_LOGI(TAG, "  Max current pins: %d", config->gpio_protection.max_current_pin_count);
    ESP_LOGI(TAG, "  Routes configured: %d", config->route_count);
    ESP_LOGI(TAG, "  Max WASM modules: %d", config->system.max_wasm_modules);
    ESP_LOGI(TAG, "  WASM heap size: %d", config->system.wasm_heap_size);
    
    return true;
}

bool mcp_config_load_from_file(mcp_config_t* config, const char* filepath) {
    if (!config || !filepath) {
        return false;
    }
    
    ESP_LOGI(TAG, "Loading MCP config from: %s", filepath);
    
    FILE* file = fopen(filepath, "r");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open MCP config file: %s (errno: %d - %s)", filepath, errno, strerror(errno));
        return false;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (size <= 0 || size > 8192) {  // Reasonable config size limit
        ESP_LOGE(TAG, "Invalid MCP config file size: %ld", size);
        fclose(file);
        return false;
    }
    
    // Read entire config file
    char* config_json = malloc(size + 1);
    if (!config_json) {
        ESP_LOGE(TAG, "Failed to allocate memory for MCP config");
        fclose(file);
        return false;
    }
    
    size_t bytes_read = fread(config_json, 1, size, file);
    fclose(file);
    config_json[bytes_read] = '\0';
    
    if (bytes_read != size) {
        ESP_LOGE(TAG, "Failed to read MCP config completely");
        free(config_json);
        return false;
    }
    
    ESP_LOGI(TAG, "MCP config file loaded (%zu bytes)", bytes_read);
    
    // Parse the JSON content
    bool result = mcp_config_load_from_json(config, config_json);
    
    free(config_json);
    return result;
}