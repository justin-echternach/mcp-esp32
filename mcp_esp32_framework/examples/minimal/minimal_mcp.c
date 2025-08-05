#include "mcp_sdk.h"
#include <stdio.h>

// Minimal MCP Module Example
// This is the simplest possible MCP module that responds to requests

// Host-provided functions needed
extern int snprintf(char* str, unsigned long size, const char* format, ...);
extern char* strcpy(char* dest, const char* src);

// Tool: hello - Simple greeting tool
mcp_result_t hello_handler(const char* params) {
    // Parse name parameter (optional)
    char name[64] = "World";
    mcp_parse_string_param(params, "name", name, sizeof(name));
    
    // Create response
    char response[256];
    snprintf(response, sizeof(response), 
        "Hello, %s!", name);
    
    return mcp_success(response);
}

// Tool: echo - Echo back the input
mcp_result_t echo_handler(const char* params) {
    char message[256] = "";
    
    if (mcp_parse_string_param(params, "message", message, sizeof(message)) != 0) {
        return mcp_error(-1, "Missing 'message' parameter");
    }
    
    char response[256];
    snprintf(response, sizeof(response),
        "You said: %s", message);
    
    return mcp_success(response);
}

// Tool: add - Add two numbers
mcp_result_t add_handler(const char* params) {
    int32_t a = 0, b = 0;
    
    if (mcp_parse_int_param(params, "a", &a) != 0) {
        return mcp_error(-1, "Missing 'a' parameter");
    }
    
    if (mcp_parse_int_param(params, "b", &b) != 0) {
        return mcp_error(-1, "Missing 'b' parameter");
    }
    
    char response[256];
    snprintf(response, sizeof(response),
        "%d + %d = %d", a, b, a + b);
    
    return mcp_success(response);
}

// Module initialization - called when WASM module is loaded
void _start() {
    // Initialize MCP server
    if (mcp_server_init("minimal_example", "1.0.0") != 0) {
        return;
    }
    
    // Register tools
    mcp_tool_t tools[3];
    
    // Tool 1: hello
    strcpy(tools[0].name, "hello");
    strcpy(tools[0].description, "Say hello with an optional name");
    tools[0].handler = hello_handler;
    
    // Tool 2: echo
    strcpy(tools[1].name, "echo");
    strcpy(tools[1].description, "Echo back a message");
    tools[1].handler = echo_handler;
    
    // Tool 3: add
    strcpy(tools[2].name, "add");
    strcpy(tools[2].description, "Add two numbers");
    tools[2].handler = add_handler;
    
    // Register all tools
    for (int i = 0; i < 3; i++) {
        mcp_register_tool(&tools[i]);
    }
}

// Required: Main MCP request handler
int32_t handle_mcp_request(const char* request, char* response, int32_t max_len) {
    // The SDK's mcp_handle_request will parse the request and call our handlers
    return mcp_handle_request(request, response, max_len);
}