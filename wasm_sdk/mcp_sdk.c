#include "mcp_sdk.h"

// Define NULL since we're using -nostdlib
#ifndef NULL
#define NULL ((void*)0)
#endif

// Host-provided string functions (don't use standard library)
// Use standard signatures to avoid compiler warnings
extern int wasm_strcmp(const char* s1, const char* s2);
extern unsigned long strlen(const char* s);
extern char* strcat(char* dest, const char* src);
extern char* strcpy(char* dest, const char* src);
extern char* wasm_strncpy(char* dest, const char* src, unsigned long n);
extern char* strstr(const char* haystack, const char* needle);
extern int strncmp(const char* s1, const char* s2, unsigned long n);

// Host-provided memory allocation functions
extern void* malloc(unsigned long size);
extern void free(void* ptr);

// Host-provided formatting function
extern int snprintf(char* buffer, unsigned long size, const char* format, ...);

// SDK Implementation for MCP WASM Modules

// Forward declarations
static int32_t handle_initialize(const char* params, char* response, int32_t max_len);
static int32_t handle_initialized(char* response, int32_t max_len);
static int32_t handle_tools_list(char* response, int32_t max_len);
static int32_t handle_tools_call(const char* params, char* response, int32_t max_len);
static int atoi(const char* str);

// Global state - using heap-allocated strings
static char* g_server_name = NULL;
static char* g_server_version = NULL;
static mcp_tool_t* g_tools = NULL;  // Heap-allocated tools array
static int g_tool_count = 0;
static mcp_state_t g_mcp_state = MCP_STATE_UNINITIALIZED;

// Initialize MCP server
int32_t mcp_server_init(const char* server_name, const char* version) {
    log_info("MCP", "mcp_server_init: Starting");
    
    if (!server_name || !version) {
        log_info("MCP", "mcp_server_init: Invalid parameters");
        return -1;
    }
    
    // Free existing allocations if any
    if (g_server_name) {
        free(g_server_name);
        g_server_name = NULL;
    }
    if (g_server_version) {
        free(g_server_version);
        g_server_version = NULL;
    }
    if (g_tools) {
        free(g_tools);
        g_tools = NULL;
    }
    
    log_info("MCP", "mcp_server_init: About to allocate server name");
    g_server_name = malloc(64);
    if (!g_server_name) {
        log_info("MCP", "mcp_server_init: Failed to allocate server name");
        return -1;
    }
    
    log_info("MCP", "mcp_server_init: About to allocate version");
    g_server_version = malloc(16);
    if (!g_server_version) {
        log_info("MCP", "mcp_server_init: Failed to allocate version");
        free(g_server_name);
        g_server_name = NULL;
        return -1;
    }
    
    log_info("MCP", "mcp_server_init: About to copy server name to heap");
    wasm_strncpy(g_server_name, server_name, 63);
    g_server_name[63] = '\0';
    log_info("MCP", "mcp_server_init: Server name copied to heap");
    
    log_info("MCP", "mcp_server_init: About to copy version to heap");
    wasm_strncpy(g_server_version, version, 15);
    g_server_version[15] = '\0';
    log_info("MCP", "mcp_server_init: Version copied to heap");
    
    log_info("MCP", "mcp_server_init: About to allocate tools array");
    g_tools = malloc(sizeof(mcp_tool_t) * 16);
    if (!g_tools) {
        log_info("MCP", "mcp_server_init: Failed to allocate tools array");
        free(g_server_name);
        free(g_server_version);
        g_server_name = NULL;
        g_server_version = NULL;
        return -1;
    }
    log_info("MCP", "mcp_server_init: Tools array allocated on heap");
    
    g_tool_count = 0;
    log_info("MCP", "mcp_server_init: Tool count reset");
    
    log_info("MCP", "Server initialized");
    return 0;
}

// Register a tool
int32_t mcp_register_tool(const mcp_tool_t* tool) {
    log_info("MCP", "mcp_register_tool: Starting");
    
    if (!tool) {
        log_info("MCP", "mcp_register_tool: NULL tool");
        return -1;
    }
    
    if (!g_tools) {
        log_info("MCP", "mcp_register_tool: Tools array not allocated");
        return -1;
    }
    
    if (g_tool_count >= 16) {
        log_info("MCP", "mcp_register_tool: Too many tools");
        return -1;
    }
    
    log_info("MCP", "mcp_register_tool: About to copy tool");
    g_tools[g_tool_count] = *tool;
    log_info("MCP", "mcp_register_tool: Tool copied");
    
    g_tool_count++;
    log_info("MCP", "mcp_register_tool: Tool count incremented");
    
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
    
    // TEST: Comment out json_parse_request and hardcode values
    if (json_parse_request(json_request, method, params, sizeof(params)) != 0) {
        log_error("MCP", "mcp_handle_request: JSON parse failed");
        return json_create_error(-32700, "Parse error", response, max_len);
    }
    
    // Hardcode method for testing
    //strcpy(method, "tools/list");
    //params[0] = '\0';  // Empty params

    //log_info("MCP - method: ", method);
    
    // Test with log_info2 - manual address handling
    //log_info2((uint32_t)"mcp_handle_request:JSONparsedsuccessfully");
    
    // Test with log_info3 - length-based handling
    //log_info3("mcp_handle_request:JSONparsedsuccessfully", 42);
    
    // Now test log_info after the others
    log_info("MCP", "mcp_handle_request:JSON parsed successfully!");
    
    // Handle different MCP methods
    log_info("MCP", "mcp_handle_request: About to check initialize");
    if (wasm_strcmp(method, "initialize") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_initialize");
        return handle_initialize(params, response, max_len);
    }
    
    log_info("MCP", "mcp_handle_request: About to check initialized");
    if (wasm_strcmp(method, "initialized") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_initialized");
        return handle_initialized(response, max_len);
    }
    
    log_info("MCP", "mcp_handle_request:About to check tools/list");
    if (wasm_strcmp(method, "tools/list") == 0) {
        log_info("MCP", "mcp_handle_request:Matched tools/list, checking state");
        if (g_mcp_state != MCP_STATE_INITIALIZED) {
            log_info("MCP", "mcp_handle_request:Server not initialized, creating error");
            return json_create_error(-32002, "Server not initialized", response, max_len);
        }
        log_info("MCP", "mcp_handle_request:Calling handle_tools_list");
        return handle_tools_list(response, max_len);
    }
    
    log_info("MCP", "mcp_handle_request:About to check tools/call");
    if (wasm_strcmp(method, "tools/call") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_tools_call");
        if (g_mcp_state != MCP_STATE_INITIALIZED) {
            return json_create_error(-32002, "Server not initialized", response, max_len);
        }
        return handle_tools_call(params, response, max_len);
    } 
    
    log_info("MCP", "mcp_handle_request: About to check ping");
    if (wasm_strcmp(method, "ping") == 0) {
        log_info("MCP", "mcp_handle_request: Calling ping handler");
        return json_create_response("{\"message\":\"pong\"}", response, max_len);
    }
    
    log_error("MCP", "mcp_handle_request: Unknown method");
    return json_create_error(-32601, "Method not found", response, max_len);
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
    
    // Use heap-allocated globals or fallback to hardcoded values
    const char* server_name = g_server_name ? g_server_name : "led_controller";
    const char* server_version = g_server_version ? g_server_version : "1.0.0";
    
    snprintf(result, sizeof(result),
        "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{"
        "\"tools\":{\"listChanged\":false},"
        "\"resources\":{\"subscribe\":false,\"listChanged\":false},"
        "\"prompts\":{\"listChanged\":false},"
        "\"logging\":{}},"
        "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}",
        server_name, server_version);
    
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
    
    // initialized is notification - return empty response (JSON-RPC 2.0 notifications have no response)
    response[0] = '\0';
    return 0;
}

// Handle tools/list request
static int32_t handle_tools_list(char* response, int32_t max_len) {
    if (!g_tools) {
        return json_create_error(-32003, "Tools array not allocated", response, max_len);
    }
    
    // Log the tool count to debug missing 4th tool
    char count_msg[64];
    snprintf(count_msg, sizeof(count_msg), "handle_tools_list: g_tool_count = %d", g_tool_count);
    log_info("MCP", count_msg);
    
    // Use the response buffer directly to avoid large stack allocation
    strcpy(response, "{\"tools\":[");
    
    for (int i = 0; i < g_tool_count; i++) {
        char tool_json[512];  // Increased size to accommodate full descriptions
        
        // Create generic tool schema using actual registered tool data
        snprintf(tool_json, sizeof(tool_json),
            "%s{\"name\":\"%s\",\"description\":\"%s\","
            "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}",
            i > 0 ? "," : "", 
            g_tools[i].name, 
            g_tools[i].description);
        
        // Check if we have space before adding
        if (strlen(response) + strlen(tool_json) + 3 < max_len) {
            strcat(response, tool_json);
        }
    }
    
    strcat(response, "]}");
    return 0;  // Success - no need to call json_create_response since we built it directly
}

// Handle tools/call request
static int32_t handle_tools_call(const char* params, char* response, int32_t max_len) {
    if (!g_tools) {
        return json_create_error(-32003, "Tools array not allocated", response, max_len);
    }
    
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
        if (wasm_strcmp(g_tools[i].name, tool_name) == 0) {
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
    wasm_strncpy(result.data, data ? data : "{}", sizeof(result.data) - 1);
    return result;
}

// Helper: Create error result
mcp_result_t mcp_error(int32_t code, const char* message) {
    mcp_result_t result;
    result.code = code;
    wasm_strncpy(result.message, message ? message : "Unknown error", sizeof(result.message) - 1);
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