# ESP32 WASM MCP SDK Architecture

## Overview
This architecture enables AI systems (like Claude) to safely control ESP32 hardware through WebAssembly modules that implement the Model Context Protocol (MCP). Each WASM module acts as an MCP server, handling JSON-RPC requests and calling ESP32 host functions for hardware access.

## Architecture Layers

### 1. HTTP Layer (ESP32 Native)
- Receives MCP JSON-RPC requests over HTTP
- Routes requests to appropriate WASM modules based on config
- Manages multiple concurrent connections and SSE streams
- Handles authentication and rate limiting

### 2. WASM Runtime Layer (ESP32 Native)
- Loads and manages multiple WASM modules
- Provides execution environment isolation
- Exports host functions to WASM modules
- Enforces GPIO protection and security policies

### 3. MCP SDK Layer (WASM Module)
- Implements MCP server protocol in WASM
- Parses JSON-RPC requests and generates responses
- Calls host functions for hardware operations
- Provides standardized API for tool implementations

### 4. Tool Implementation Layer (WASM Module)
- Implements specific tools (LED control, GPIO, sensors, etc.)
- Uses MCP SDK for protocol handling
- Calls host functions through MCP SDK wrappers

## Host Function Interface

The ESP32 exports these functions to WASM modules:

```c
// GPIO Control
int32_t gpio_set_level(int32_t pin, int32_t level);
int32_t gpio_get_level(int32_t pin);
int32_t gpio_set_direction(int32_t pin, int32_t direction);
int32_t gpio_is_pin_protected(int32_t pin);

// SSE Events
int32_t sse_send_event(const char* event_type, const char* data);
int32_t sse_send_tool_result(const char* tool_name, const char* result);

// System Info
int32_t get_tick_count(void);
int32_t get_free_heap(void);
int32_t get_uptime_ms(void);

// JSON Helper Functions
int32_t json_parse_request(const char* json, char* method, char* params, int32_t max_len);
int32_t json_create_response(const char* result, char* response, int32_t max_len);
int32_t json_create_error(int32_t code, const char* message, char* response, int32_t max_len);

// Logging
void log_info(const char* tag, const char* message);
void log_error(const char* tag, const char* message);
```

## WASM MCP SDK API

WASM modules use this SDK to implement MCP servers:

```c
// MCP Server Framework
typedef struct {
    const char* name;
    const char* description;
    mcp_tool_handler_t handler;
} mcp_tool_t;

typedef struct {
    int32_t code;
    const char* message;
    const char* data;
} mcp_result_t;

// Core MCP Functions
int32_t mcp_server_init(const char* server_name, const char* version);
int32_t mcp_register_tool(const mcp_tool_t* tool);
int32_t mcp_handle_request(const char* json_request, char* response, int32_t max_len);

// Tool Handler Signature
typedef mcp_result_t (*mcp_tool_handler_t)(const char* params);

// Helper Functions
mcp_result_t mcp_success(const char* data);
mcp_result_t mcp_error(int32_t code, const char* message);
```

## Example WASM Module Implementation

```c
// led_controller.wasm implementation
#include "mcp_sdk.h"

// Tool: control_led
mcp_result_t led_control_handler(const char* params) {
    // Parse JSON params to get pin and state
    int32_t pin, state;
    if (parse_led_params(params, &pin, &state) != 0) {
        return mcp_error(-1, "Invalid parameters");
    }
    
    // Check if pin is protected
    if (gpio_is_pin_protected(pin)) {
        return mcp_error(-2, "Pin is protected");
    }
    
    // Set GPIO level
    if (gpio_set_level(pin, state) != 0) {
        return mcp_error(-3, "GPIO operation failed");
    }
    
    // Send SSE event
    char event_data[64];
    snprintf(event_data, sizeof(event_data), "{\"pin\":%d,\"state\":%d}", pin, state);
    sse_send_event("gpio_change", event_data);
    
    return mcp_success(event_data);
}

// Tool: get_led_status  
mcp_result_t led_status_handler(const char* params) {
    int32_t pin;
    if (parse_pin_param(params, &pin) != 0) {
        return mcp_error(-1, "Invalid pin parameter");
    }
    
    int32_t level = gpio_get_level(pin);
    char result[32];
    snprintf(result, sizeof(result), "{\"pin\":%d,\"level\":%d}", pin, level);
    
    return mcp_success(result);
}

// Module initialization
void _start() {
    mcp_server_init("led_controller", "1.0.0");
    
    mcp_tool_t led_tools[] = {
        {
            .name = "control_led",
            .description = "Control LED state on specified GPIO pin",
            .handler = led_control_handler
        },
        {
            .name = "get_led_status",
            .description = "Get current LED state from GPIO pin", 
            .handler = led_status_handler
        }
    };
    
    for (int i = 0; i < 2; i++) {
        mcp_register_tool(&led_tools[i]);
    }
}

// Main MCP request handler (called by ESP32)
int32_t handle_mcp_request(const char* request, char* response, int32_t max_len) {
    return mcp_handle_request(request, response, max_len);
}
```

## Security and Protection

1. **GPIO Protection**: Host functions check `gpio_protection_config` before allowing operations
2. **Execution Isolation**: Each WASM module runs in isolated memory space
3. **Resource Limits**: WASM modules have limited heap and stack size
4. **Timeout Protection**: MCP requests have configurable timeouts
5. **Rate Limiting**: HTTP layer enforces request rate limits

## Configuration-Driven Routing

The `cfg.jsn` file maps HTTP routes to WASM modules:

```json
{
  "routes": {
    "/mcp": "default_mcp.wasm",
    "/mcp/led": "led_controller.wasm", 
    "/mcp/gpio": "gpio_control.wasm",
    "/mcp/system": "system_info.wasm"
  }
}
```

## Request Flow

1. HTTP request arrives at `/mcp/led` with JSON-RPC body
2. ESP32 routing layer loads `led_controller.wasm` if not already loaded
3. ESP32 calls `handle_mcp_request()` in WASM module with JSON payload
4. WASM module parses request using MCP SDK
5. WASM module calls appropriate tool handler
6. Tool handler calls ESP32 host functions (gpio_set_level, etc.)
7. Host functions check GPIO protection before executing
8. WASM module returns JSON-RPC response
9. ESP32 sends HTTP response and SSE events if configured

This architecture provides safe, isolated, and configurable AI control of ESP32 hardware while maintaining the flexibility to implement diverse tools and capabilities.