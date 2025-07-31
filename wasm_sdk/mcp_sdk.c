#include "mcp_sdk.h"
#include <string.h>
#include <stdio.h>

// SDK Implementation for MCP WASM Modules

// Forward declarations
static int32_t handle_initialize(const char* params, char* response, int32_t max_len);
static int32_t handle_initialized(char* response, int32_t max_len);
static int32_t handle_tools_list(char* response, int32_t max_len);
static int32_t handle_tools_call(const char* params, char* response, int32_t max_len);
static int atoi(const char* str);

// Global state
static char g_server_name[64] = "";
static char g_server_version[16] = "";
static mcp_tool_t g_tools[16];  // Max 16 tools per module
static int g_tool_count = 0;
static mcp_state_t g_mcp_state = MCP_STATE_UNINITIALIZED;

// Initialize MCP server
int32_t mcp_server_init(const char* server_name, const char* version) {
    if (!server_name || !version) return -1;
    
    strncpy(g_server_name, server_name, sizeof(g_server_name) - 1);
    strncpy(g_server_version, version, sizeof(g_server_version) - 1);
    g_tool_count = 0;
    
    log_info("MCP", "Server initialized");
    return 0;
}

// Register a tool
int32_t mcp_register_tool(const mcp_tool_t* tool) {
    if (!tool || g_tool_count >= 16) return -1;
    
    g_tools[g_tool_count] = *tool;
    g_tool_count++;
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Registered tool: %s", tool->name);
    log_info("MCP", msg);
    
    return 0;
}

// Handle MCP JSON-RPC request
int32_t mcp_handle_request(const char* json_request, char* response, int32_t max_len) {
    log_info("MCP", "mcp_handle_request: Starting");
    
    char method[64];
    char params[512];
    
    // Parse JSON-RPC request
    log_info("MCP", "mcp_handle_request: About to parse JSON");
    if (json_parse_request(json_request, method, params, sizeof(params)) != 0) {
        log_error("MCP", "mcp_handle_request: JSON parse failed");
        return json_create_error(-32700, "Parse error", response, max_len);
    }
    
    log_info("MCP", "mcp_handle_request: JSON parsed successfully");
    
    // Handle different MCP methods
    if (strcmp(method, "initialize") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_initialize");
        return handle_initialize(params, response, max_len);
    } else if (strcmp(method, "initialized") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_initialized");
        return handle_initialized(response, max_len);
    } else if (strcmp(method, "tools/list") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_tools_list");
        if (g_mcp_state != MCP_STATE_INITIALIZED) {
            return json_create_error(-32002, "Server not initialized", response, max_len);
        }
        return handle_tools_list(response, max_len);
    } else if (strcmp(method, "tools/call") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_tools_call");
        if (g_mcp_state != MCP_STATE_INITIALIZED) {
            return json_create_error(-32002, "Server not initialized", response, max_len);
        }
        return handle_tools_call(params, response, max_len);
    } else if (strcmp(method, "ping") == 0) {
        log_info("MCP", "mcp_handle_request: Calling ping handler");
        return json_create_response("{\"message\":\"pong\"}", response, max_len);
    } else {
        log_error("MCP", "mcp_handle_request: Unknown method");
        return json_create_error(-32601, "Method not found", response, max_len);
    }
}

// Handle initialize request
static int32_t handle_initialize(const char* params, char* response, int32_t max_len) {
    log_info("MCP", "handle_initialize: Starting");
    
    g_mcp_state = MCP_STATE_INITIALIZING;
    log_info("MCP", "handle_initialize: State set to initializing");
    
    // Parse client info from params if needed (protocol version, etc.)
    // For now, just accept any initialize request
    
    log_info("MCP", "handle_initialize: About to create result JSON");
    char result[512];
    snprintf(result, sizeof(result),
        "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{"
        "\"tools\":{\"listChanged\":false},"
        "\"resources\":{\"subscribe\":false,\"listChanged\":false},"
        "\"prompts\":{\"listChanged\":false},"
        "\"logging\":{}},"
        "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}",
        g_server_name, g_server_version);
    
    log_info("MCP", "handle_initialize: JSON created, about to call json_create_response");
    log_info("MCP", "Initialize request received");
    return json_create_response(result, response, max_len);
}

// Handle initialized notification
static int32_t handle_initialized(char* response, int32_t max_len) {
    if (g_mcp_state != MCP_STATE_INITIALIZING) {
        return json_create_error(-32002, "Server not in initializing state", response, max_len);
    }
    
    g_mcp_state = MCP_STATE_INITIALIZED;
    log_info("MCP", "Server initialized successfully");
    
    // initialized is a notification, so no response content needed
    response[0] = '\0';
    return 0;
}

// Handle tools/list request
static int32_t handle_tools_list(char* response, int32_t max_len) {
    char tools_json[1024] = "{\"tools\":[";
    
    for (int i = 0; i < g_tool_count; i++) {
        char tool_json[512];
        
        // Create tool schema based on tool name
        if (strcmp(g_tools[i].name, "control_led") == 0) {
            snprintf(tool_json, sizeof(tool_json),
                "%s{\"name\":\"%s\",\"description\":\"%s\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                "\"pin\":{\"type\":\"number\",\"description\":\"GPIO pin number (0-39)\"},"
                "\"state\":{\"type\":\"number\",\"description\":\"LED state (0=off, 1=on)\"}"
                "},\"required\":[\"pin\",\"state\"]}}",
                i > 0 ? "," : "", g_tools[i].name, g_tools[i].description);
        } else if (strcmp(g_tools[i].name, "get_led_status") == 0 || strcmp(g_tools[i].name, "toggle_led") == 0) {
            snprintf(tool_json, sizeof(tool_json),
                "%s{\"name\":\"%s\",\"description\":\"%s\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                "\"pin\":{\"type\":\"number\",\"description\":\"GPIO pin number (0-39)\"}"
                "},\"required\":[\"pin\"]}}",
                i > 0 ? "," : "", g_tools[i].name, g_tools[i].description);
        } else {
            // Default schema for tools without parameters
            snprintf(tool_json, sizeof(tool_json),
                "%s{\"name\":\"%s\",\"description\":\"%s\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}",
                i > 0 ? "," : "", g_tools[i].name, g_tools[i].description);
        }
        
        if (strlen(tools_json) + strlen(tool_json) + 3 < sizeof(tools_json)) {
            strcat(tools_json, tool_json);
        }
    }
    
    strcat(tools_json, "]}");
    return json_create_response(tools_json, response, max_len);
}

// Handle tools/call request
static int32_t handle_tools_call(const char* params, char* response, int32_t max_len) {
    char tool_name[64];
    char tool_params[256];
    
    // Parse tool name from params
    if (mcp_parse_string_param(params, "name", tool_name, sizeof(tool_name)) != 0) {
        return json_create_error(-32602, "Missing tool name", response, max_len);
    }
    
    // Parse tool arguments (if any)
    if (mcp_parse_string_param(params, "arguments", tool_params, sizeof(tool_params)) != 0) {
        strcpy(tool_params, "{}"); // Default empty arguments
    }
    
    // Find and call the tool
    for (int i = 0; i < g_tool_count; i++) {
        if (strcmp(g_tools[i].name, tool_name) == 0) {
            mcp_result_t result = g_tools[i].handler(tool_params);
            
            if (result.code == 0) {
                // Send SSE event for successful tool execution
                sse_send_tool_result(tool_name, result.data);
                return json_create_response(result.data, response, max_len);
            } else {
                return json_create_error(result.code, result.message, response, max_len);
            }
        }
    }
    
    return json_create_error(-32601, "Tool not found", response, max_len);
}

// Helper: Create success result
mcp_result_t mcp_success(const char* data) {
    mcp_result_t result;
    result.code = 0;
    strcpy(result.message, "Success");
    strncpy(result.data, data ? data : "{}", sizeof(result.data) - 1);
    return result;
}

// Helper: Create error result
mcp_result_t mcp_error(int32_t code, const char* message) {
    mcp_result_t result;
    result.code = code;
    strncpy(result.message, message ? message : "Unknown error", sizeof(result.message) - 1);
    strcpy(result.data, "null");
    return result;
}

// JSON parameter parsing helpers
int32_t mcp_parse_int_param(const char* params, const char* key, int32_t* value) {
    // Simple JSON integer parsing
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    const char* found = strstr(params, search_key);
    if (!found) return -1;
    
    found += strlen(search_key);
    while (*found == ' ' || *found == '\t') found++; // Skip whitespace
    
    *value = atoi(found);
    return 0;
}

int32_t mcp_parse_string_param(const char* params, const char* key, char* value, int32_t max_len) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    const char* found = strstr(params, search_key);
    if (!found) return -1;
    
    found += strlen(search_key);
    while (*found == ' ' || *found == '\t') found++; // Skip whitespace
    
    if (*found != '"') return -1; // Not a string
    found++; // Skip opening quote
    
    int i = 0;
    while (i < max_len - 1 && *found && *found != '"') {
        if (*found == '\\' && *(found + 1)) {
            found++; // Skip escape character
        }
        value[i++] = *found++;
    }
    value[i] = '\0';
    
    return 0;
}

int32_t mcp_parse_bool_param(const char* params, const char* key, bool* value) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    const char* found = strstr(params, search_key);
    if (!found) return -1;
    
    found += strlen(search_key);
    while (*found == ' ' || *found == '\t') found++; // Skip whitespace
    
    if (strncmp(found, "true", 4) == 0) {
        *value = true;
        return 0;
    } else if (strncmp(found, "false", 5) == 0) {
        *value = false;
        return 0;
    }
    
    return -1;
}

// JSON creation helpers
int32_t mcp_create_json_int(const char* key, int32_t value, char* json, int32_t max_len) {
    return snprintf(json, max_len, "{\"%s\":%d}", key, value);
}

int32_t mcp_create_json_string(const char* key, const char* value, char* json, int32_t max_len) {
    return snprintf(json, max_len, "{\"%s\":\"%s\"}", key, value);
}

int32_t mcp_create_json_bool(const char* key, bool value, char* json, int32_t max_len) {
    return snprintf(json, max_len, "{\"%s\":%s}", key, value ? "true" : "false");
}

// Simple atoi implementation (if not available)
static int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    if (*str == '-') {
        sign = -1;
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}