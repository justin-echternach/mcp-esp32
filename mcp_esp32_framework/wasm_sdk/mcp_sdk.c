#include "mcp_sdk.h"

// Define NULL since we're using -nostdlib
#ifndef NULL
#define NULL ((void*)0)
#endif



// SDK Implementation for MCP WASM Modules

// Forward declarations
static int32_t handle_initialize(const char* params, char* response, int32_t max_len);
static int32_t handle_initialized(char* response, int32_t max_len);
static int32_t handle_tools_list(char* response, int32_t max_len);
static int32_t handle_tools_call(const char* params, char* response, int32_t max_len);
static int32_t extract_json_id(const char* json);
static int32_t create_jsonrpc_response(const char* result, char* response, int32_t max_len);
static int32_t create_jsonrpc_error(int32_t code, const char* message, char* response, int32_t max_len);

// Global state - using heap-allocated strings
static char* g_server_name = NULL;
static char* g_server_version = NULL;
static mcp_tool_t* g_tools = NULL;  // Heap-allocated tools array
static int g_tool_count = 0;
static mcp_state_t g_mcp_state = MCP_STATE_UNINITIALIZED;
static int32_t g_current_request_id = 0;  // Store current JSON-RPC request ID

// Escape text for use inside a JSON string literal. Always NUL-terminates
// (truncating if needed). Tool results, descriptions and error messages are
// plain text, so they must go through this before being placed in "...".
static void json_escape(const char* in, char* out, int32_t out_size) {
    static const char hex[] = "0123456789abcdef";
    int32_t o = 0;
    if (out_size <= 0) return;
    for (; in && *in; in++) {
        unsigned char c = (unsigned char)*in;
        char esc[6];
        int32_t n = 0;
        if (c == '"' || c == '\\') {
            esc[n++] = '\\'; esc[n++] = (char)c;
        } else if (c == '\n') {
            esc[n++] = '\\'; esc[n++] = 'n';
        } else if (c == '\r') {
            esc[n++] = '\\'; esc[n++] = 'r';
        } else if (c == '\t') {
            esc[n++] = '\\'; esc[n++] = 't';
        } else if (c < 0x20) {
            esc[n++] = '\\'; esc[n++] = 'u'; esc[n++] = '0'; esc[n++] = '0';
            esc[n++] = hex[c >> 4]; esc[n++] = hex[c & 0xf];
        } else {
            esc[n++] = (char)c;
        }
        if (o + n >= out_size) break;
        for (int32_t k = 0; k < n; k++) out[o++] = esc[k];
    }
    out[o] = '\0';
}

// Buffer allocation helpers for dynamic memory management
static char* alloc_mcp_buffer(unsigned long size, const char* name) {
    char* buffer = malloc(size);
    if (!buffer) {
        log_error("MCP", "Failed to allocate buffer");
    }
    return buffer;
}

static void free_mcp_buffer(char* buffer) {
    if (buffer) {
        free(buffer);
    }
}

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
    strncpy(g_server_name, server_name, 63);
    g_server_name[63] = '\0';
    log_info("MCP", "mcp_server_init: Server name copied to heap");
    
    log_info("MCP", "mcp_server_init: About to copy version to heap");
    strncpy(g_server_version, version, 15);
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
    char* params = alloc_mcp_buffer(MCP_PARAMS_BUFFER_SIZE, "params");
    if (!params) {
        g_current_request_id = 0; // Default ID for allocation errors
        return create_jsonrpc_error(-32000, "Memory allocation failed", response, max_len);
    }
    
    // Parse JSON-RPC request
    log_info("MCP", "mcp_handle_request: About to parse JSON");
    
    // TEST: Comment out json_parse_request and hardcode values
    if (json_parse_request(json_request, method, params, MCP_PARAMS_BUFFER_SIZE) != 0) {
        log_error("MCP", "mcp_handle_request: JSON parse failed");
        g_current_request_id = 0; // Default ID for parse errors
        free_mcp_buffer(params);
        return create_jsonrpc_error(-32700, "Parse error", response, max_len);
    }
    
    // Extract the JSON-RPC ID after successful parse
    g_current_request_id = extract_json_id(json_request);
    
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
    int32_t result;
    
    log_info("MCP", "mcp_handle_request: About to check initialize");
    if (strcmp(method, "initialize") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_initialize");
        result = handle_initialize(params, response, max_len);
        free_mcp_buffer(params);
        return result;
    }
    
    log_info("MCP", "mcp_handle_request: About to check initialized");
    if (strcmp(method, "notifications/initialized") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_initialized");
        result = handle_initialized(response, max_len);
        free_mcp_buffer(params);
        return result;
    }
    
    log_info("MCP", "mcp_handle_request:About to check tools/list");
    if (strcmp(method, "tools/list") == 0) {
        log_info("MCP", "mcp_handle_request:Matched tools/list, checking state");
        if (g_mcp_state != MCP_STATE_INITIALIZED) {
            log_info("MCP", "mcp_handle_request:Server not initialized, creating error");
            result = create_jsonrpc_error(-32002, "Server not initialized", response, max_len);
            free_mcp_buffer(params);
            return result;
        }
        log_info("MCP", "mcp_handle_request:Calling handle_tools_list");
        result = handle_tools_list(response, max_len);
        free_mcp_buffer(params);
        return result;
    }
    
    log_info("MCP", "mcp_handle_request:About to check tools/call");
    if (strcmp(method, "tools/call") == 0) {
        log_info("MCP", "mcp_handle_request: Calling handle_tools_call");
        if (g_mcp_state != MCP_STATE_INITIALIZED) {
            result = create_jsonrpc_error(-32002, "Server not initialized", response, max_len);
            free_mcp_buffer(params);
            return result;
        }
        result = handle_tools_call(params, response, max_len);
        free_mcp_buffer(params);
        return result;
    } 
    
    log_info("MCP", "mcp_handle_request: About to check prompts/list");
    if (strcmp(method, "prompts/list") == 0) {
        log_info("MCP", "mcp_handle_request: Calling prompts/list handler");
        // Return empty prompts array - we don't implement prompts
        result = create_jsonrpc_response("{\"prompts\":[]}", response, max_len);
        free_mcp_buffer(params);
        return result;
    }
    
    log_info("MCP", "mcp_handle_request: About to check ping");
    if (strcmp(method, "ping") == 0) {
        log_info("MCP", "mcp_handle_request: Calling ping handler");
        result = create_jsonrpc_response("{\"message\":\"pong\"}", response, max_len);
        free_mcp_buffer(params);
        return result;
    }
    
    log_error("MCP", "mcp_handle_request: Unknown method");
    result = create_jsonrpc_error(-32601, "Method not found", response, max_len);
    free_mcp_buffer(params);
    return result;
}

// Handle initialize request
static int32_t handle_initialize(const char* params, char* response, int32_t max_len) {
    log_info("MCP", "handle_initialize: Starting");
    
    g_mcp_state = MCP_STATE_INITIALIZING;
    log_info("MCP", "handle_initialize: State set to initializing");
    
    // Generate session ID and set response header
    char session_id[64];
    mcp_generate_session_id(session_id, sizeof(session_id));
    
    if (mcp_set_session_header(session_id) != 0) {
        log_error("MCP", "Failed to set session header");
        return create_jsonrpc_error(-32000, "Failed to set session header", response, max_len);
    }
    
    log_info("MCP", "Session ID generated and header set");
    
    // Parse client info from params if needed (protocol version, etc.)
    // For now, just accept any initialize request
    
    log_info("MCP", "handle_initialize: About to create result JSON");
    char result[512];
    
    // Use heap-allocated globals or fallback to hardcoded values
    const char* server_name = g_server_name ? g_server_name : "led_controller";
    const char* server_version = g_server_version ? g_server_version : "1.0.0";
    
    snprintf(result, sizeof(result),
        "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{"
        "\"tools\":{\"listChanged\":false},"
        "\"resources\":{\"subscribe\":false,\"listChanged\":false},"
        "\"prompts\":{\"listChanged\":false},"
        "\"logging\":{}},"
        "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}",
        server_name, server_version);
    
    log_info("MCP", "handle_initialize: JSON created, about to call json_create_response");
    log_info("MCP", "Initialize request received");
    
    // Create response with correct ID
    int len = snprintf(response, max_len,
        "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%d}",
        result, g_current_request_id);
    
    return len < max_len ? 0 : -2;
}

// Handle initialized notification
static int32_t handle_initialized(char* response, int32_t max_len) {
    if (g_mcp_state != MCP_STATE_INITIALIZING) {
        return create_jsonrpc_error(-32002, "Server not in initializing state", response, max_len);
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
        return create_jsonrpc_error(-32003, "Tools array not allocated", response, max_len);
    }
    
    // Log the tool count to debug missing 4th tool
    // char count_msg[64];
    // snprintf(count_msg, sizeof(count_msg), "handle_tools_list: g_tool_count = %d", g_tool_count);
    // log_info("MCP", count_msg);
    
    // Use snprintf with position tracking instead of strcat

    char tools_result[2048];
    int pos = 0;
    int written = snprintf(tools_result, sizeof(tools_result), "{\"tools\":[");
    if (written < 0 || written >= sizeof(tools_result)) {
        return create_jsonrpc_error(-32000, "Response buffer too small", response, max_len);
    }
    pos = written;
    
    for (int i = 0; i < g_tool_count; i++) {
        char description[MCP_MAX_DESCRIPTION * 2];
        json_escape(g_tools[i].description, description, sizeof(description));

        // Build tool JSON directly into response buffer
        written = snprintf(tools_result + pos, sizeof(tools_result) - pos,
            "%s{\"name\":\"%s\",\"title\":\"%s\",\"description\":\"%s\","
            "\"inputSchema\":%s}",
            i > 0 ? "," : "", 
            g_tools[i].name, 
            g_tools[i].name, 
            description,
            g_tools[i].input_schema[0] ? g_tools[i].input_schema : "{\"type\":\"object\"}");
        
        if (written < 0 || written >= (int)sizeof(tools_result) - pos) {
            return create_jsonrpc_error(-32000, "Tool list too large for buffer", response, max_len);
        }
        pos += written;
    }
    
    // Close JSON array and object
    written = snprintf(tools_result + pos, sizeof(tools_result) - pos, "]}");
    if (written < 0 || written >= sizeof(tools_result) - pos) {
        return create_jsonrpc_error(-32000, "Response buffer overflow", response, max_len);
    }
    
    // Wrap in JSON-RPC response with correct ID
    
    //strcpy(tools_result, response);
    
    int len = snprintf(response, max_len,
        "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%d}",
        tools_result, g_current_request_id);
    
    return len < max_len ? 0 : -2;
}

// Handle tools/call request
static int32_t handle_tools_call(const char* params, char* response, int32_t max_len) {
    if (!g_tools) {
        return create_jsonrpc_error(-32003, "Tools array not allocated", response, max_len);
    }
    
    char tool_name[64];
    char* tool_params = alloc_mcp_buffer(MCP_TOOL_PARAMS_BUFFER_SIZE, "tool_params");
    if (!tool_params) {
        return create_jsonrpc_error(-32000, "Memory allocation failed", response, max_len);
    }
    
    // Parse tool name from params
    if (mcp_parse_string_param(params, "name", tool_name, sizeof(tool_name)) != 0) {
        free_mcp_buffer(tool_params);
        return create_jsonrpc_error(-32602, "Missing tool name", response, max_len);
    }
    
    // Parse tool arguments (extract JSON object for "arguments")
    const char* args_start = strstr(params, "\"arguments\":");
    if (args_start) {
        args_start += strlen("\"arguments\":");
        // Skip whitespace
        while (*args_start == ' ' || *args_start == '\t') args_start++;
        
        // Find the matching closing brace for the arguments object
        if (*args_start == '{') {
            int brace_count = 1;
            const char* p = args_start + 1;
            char* dest = tool_params;
            *dest++ = '{';
            
            while (*p && brace_count > 0 && (dest - tool_params) < (MCP_TOOL_PARAMS_BUFFER_SIZE - 2)) {
                if (*p == '{') brace_count++;
                else if (*p == '}') brace_count--;
                *dest++ = *p;
                if (brace_count == 0) break;
                p++;
            }
            *dest = '\0';
        } else {
            strcpy(tool_params, "{}");
        }
    } else {
        strcpy(tool_params, "{}"); // Default empty arguments
    }
    
    // Find and call the tool
    for (int i = 0; i < g_tool_count; i++) {
        if (strcmp(g_tools[i].name, tool_name) == 0) {
            mcp_result_t result = g_tools[i].handler(tool_params);
            
            if (result.code == 0) {
                // Send SSE event for successful tool execution
                sse_send_tool_result(tool_name, result.data);
                
                // Format response according to MCP spec (result.data is plain text)
                char text[sizeof(result.data) * 2];
                json_escape(result.data, text, sizeof(text));
                char mcp_result[1280];
                snprintf(mcp_result, sizeof(mcp_result),
                    "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":false}",
                    text);
                int32_t response_result = create_jsonrpc_response(mcp_result, response, max_len);
                free_mcp_buffer(tool_params);
                return response_result;
            } else {
                // Format error response according to MCP spec
                char text[sizeof(result.message) * 2];
                json_escape(result.message, text, sizeof(text));
                char mcp_error[640];
                snprintf(mcp_error, sizeof(mcp_error),
                    "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":true}",
                    text);
                int32_t response_result = create_jsonrpc_response(mcp_error, response, max_len);
                free_mcp_buffer(tool_params);
                return response_result;
            }
        }
    }
    
    free_mcp_buffer(tool_params);
    return create_jsonrpc_error(-32601, "Tool not found", response, max_len);
}

// Helper: Create success result
mcp_result_t mcp_success(const char* data) {
    mcp_result_t result;
    result.code = 0;
    strcpy(result.message, "Success");
    strncpy(result.data, data ? data : "{}", sizeof(result.data) - 1);
    result.data[sizeof(result.data) - 1] = '\0';
    return result;
}

// Helper: Create error result
mcp_result_t mcp_error(int32_t code, const char* message) {
    mcp_result_t result;
    result.code = code;
    strncpy(result.message, message ? message : "Unknown error", sizeof(result.message) - 1);
    result.message[sizeof(result.message) - 1] = '\0';
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

// Extract JSON-RPC ID from request
static int32_t extract_json_id(const char* json) {
    log_info("MCP", "extract_json_id: Starting");
    
    const char* id_start = strstr(json, "\"id\"");
    if (!id_start) {
        log_info("MCP", "extract_json_id: No id found");
        return 0; // Default to 0 if no ID found
    }
    
    log_info("MCP", "extract_json_id: Found id field");
    
    // Log the pointer address returned by strstr
    char addr_msg[64];
    snprintf(addr_msg, sizeof(addr_msg), "extract_json_id: id_start addr=0x%x", (uint32_t)id_start);
    log_info("MCP", addr_msg);
    
    // Add bounds check - don't go more than 10 chars looking for ':'
    int search_limit = 10;
    while (*id_start && *id_start != ':' && search_limit > 0) {
        log_info("MCP", "extract_json_id: Searching for colon");
        char debug_msg[64];
        snprintf(debug_msg, sizeof(debug_msg), "extract_json_id: char='%c' limit=%d", *id_start, search_limit);
        log_info("MCP", debug_msg);
        id_start++;
        search_limit--;
    }
    if (!*id_start || search_limit == 0) {
        log_info("MCP", "extract_json_id: No colon found");
        return 0;
    }

    log_info("MCP", "extract_json_id: Found colon, about to parse number");
    
    id_start++; // Skip ':'
    // Skip whitespace
    while (*id_start && (*id_start == ' ' || *id_start == '\t' || *id_start == '\n')) {
        log_info("MCP", "extract_json_id: Skipping whitespace");
        id_start++;
    }
    
    log_info("MCP", "extract_json_id: About to parse number");
    
    // Parse the ID (assuming it's a number)
    return atoi(id_start);
}

// Create JSON-RPC response with correct ID
static int32_t create_jsonrpc_response(const char* result, char* response, int32_t max_len) {
    int len = snprintf(response, max_len,
        "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%d}",
        result, g_current_request_id);
    return len < max_len ? 0 : -2;
}

// Create JSON-RPC error with correct ID
static int32_t create_jsonrpc_error(int32_t code, const char* message, char* response, int32_t max_len) {
    char escaped[256];
    json_escape(message, escaped, sizeof(escaped));
    int len = snprintf(response, max_len,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%d}",
        code, escaped, g_current_request_id);
    return len < max_len ? 0 : -2;
}

// ========== Session and Header Management Helpers ==========

// Generate a simple session ID (UUID-like format)
void mcp_generate_session_id(char* buffer, int32_t size) {
    if (!buffer || size < 37) return;
    
    // Use system tick as a simple pseudo-random source
    uint32_t tick = get_tick_count();
    uint32_t heap = get_free_heap();
    uint32_t uptime = get_uptime_ms();
    
    // Create a UUID-like format (not truly random, but unique enough for testing)
    snprintf(buffer, size, 
             "%08x-%04x-%04x-%04x-%08x%04x",
             tick,
             (uint16_t)(heap & 0xFFFF),
             (uint16_t)((heap >> 16) & 0xFFFF),
             (uint16_t)(uptime & 0xFFFF),
             uptime,
             (uint16_t)((tick >> 16) & 0xFFFF));
}

// Set MCP-Session-Id header in response
int32_t mcp_set_session_header(const char* session_id) {
    if (!session_id) return -1;
    
    return set_response_header("Mcp-Session-Id", session_id);
}