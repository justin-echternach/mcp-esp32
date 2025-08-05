#ifndef MCP_CONFIG_LOADER_H
#define MCP_CONFIG_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include "mcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== JSON Parsing Utilities ==========

/**
 * @brief Find a string value in JSON by key
 * 
 * @param json JSON string to search
 * @param key Key to find
 * @param value Buffer to store the found value
 * @param max_len Maximum length of value buffer
 * @return true if key was found and value extracted
 */
bool mcp_json_find_string(const char* json, const char* key, char* value, size_t max_len);

/**
 * @brief Find an integer value in JSON by key
 * 
 * @param json JSON string to search
 * @param key Key to find
 * @param value Pointer to store the found integer value
 * @return true if key was found and value extracted
 */
bool mcp_json_find_int(const char* json, const char* key, int* value);

/**
 * @brief Find a boolean value in JSON by key
 * 
 * @param json JSON string to search
 * @param key Key to find
 * @param value Pointer to store the found boolean value
 * @return true if key was found and value extracted
 */
bool mcp_json_find_bool(const char* json, const char* key, bool* value);

/**
 * @brief Parse JSON array of integers
 * 
 * @param json JSON string containing the array
 * @param key Key of the array to parse
 * @param array Buffer to store parsed integers
 * @param max_count Maximum number of integers to parse
 * @return Number of integers successfully parsed
 */
int mcp_json_parse_int_array(const char* json, const char* key, int* array, int max_count);

// ========== Configuration Loading ==========

/**
 * @brief Load MCP configuration from JSON file
 * 
 * @param config Pointer to configuration structure to populate
 * @param filepath Path to JSON configuration file
 * @return true if configuration was loaded successfully
 */
bool mcp_config_load_from_file(mcp_config_t* config, const char* filepath);

/**
 * @brief Initialize MCP configuration with defaults
 * 
 * @param config Pointer to configuration structure to initialize
 * @return true if configuration was initialized successfully
 */
bool mcp_config_init_defaults(mcp_config_t* config);

/**
 * @brief Load MCP configuration from JSON string
 * 
 * @param config Pointer to configuration structure to populate
 * @param json_string JSON configuration string
 * @return true if configuration was parsed successfully
 */
bool mcp_config_load_from_json(mcp_config_t* config, const char* json_string);

/**
 * @brief Initialize MCP configuration with smart fallback loading
 * 
 * This function tries multiple sources in order:
 * 1. SD card config files (/sdcard/cfg.jsn, /sdcard/mcp_config.json)
 * 2. Embedded config files (/spiffs/cfg.jsn)
 * 3. Default configuration with fallback route
 * 
 * @param config Pointer to configuration structure to populate
 * @param default_route Default route to use if no config found (can be NULL)
 * @param default_wasm_file Default WASM file for the route (can be NULL)
 * @return true if configuration was initialized successfully
 */
bool mcp_config_init_with_fallback(mcp_config_t* config, 
                                   const char* default_route, 
                                   const char* default_wasm_file);

#ifdef __cplusplus
}
#endif

#endif // MCP_CONFIG_LOADER_H