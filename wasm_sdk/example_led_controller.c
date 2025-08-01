#include "mcp_sdk.h"
#include <stdio.h>

// Example LED Controller MCP Module
// This demonstrates how to implement MCP tools using the SDK

// Tool: control_led - Controls LED state on specified GPIO pin
mcp_result_t control_led_handler(const char* params) {
    int32_t pin, state;
    char result_json[256];
    
    // Parse parameters
    if (mcp_parse_int_param(params, "pin", &pin) != 0) {
        return mcp_error(-1, "Missing 'pin' parameter");
    }
    
    if (mcp_parse_int_param(params, "state", &state) != 0) {
        return mcp_error(-1, "Missing 'state' parameter");
    }
    
    // Validate pin range
    if (pin < 0 || pin > 39) {
        return mcp_error(-2, "Invalid pin number (0-39)");
    }
    
    // Validate state
    if (state != 0 && state != 1) {
        return mcp_error(-3, "Invalid state (0 or 1)");
    }
    
    // Check if pin is protected
    if (gpio_is_pin_protected(pin)) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Pin %d is protected", pin);
        return mcp_error(-4, error_msg);
    }
    
    // Set GPIO direction to output
    if (gpio_set_direction(pin, GPIO_MODE_OUTPUT) != 0) {
        return mcp_error(-5, "Failed to set GPIO direction");
    }
    
    // Set GPIO level
    if (gpio_set_level(pin, state) != 0) {
        return mcp_error(-6, "Failed to set GPIO level");
    }
    
    // Create success response
    snprintf(result_json, sizeof(result_json), 
        "{\"pin\":%d,\"state\":%d,\"message\":\"LED %s\"}", 
        pin, state, state ? "ON" : "OFF");
    
    // Send SSE event
    char event_data[128];
    snprintf(event_data, sizeof(event_data), "{\"pin\":%d,\"state\":%d}", pin, state);
    sse_send_event("gpio_change", event_data);
    
    log_info("LED", result_json);
    return mcp_success(result_json);
}

// Tool: get_led_status - Gets current LED state from GPIO pin
mcp_result_t get_led_status_handler(const char* params) {
    int32_t pin;
    char result_json[256];
    
    // Parse pin parameter
    if (mcp_parse_int_param(params, "pin", &pin) != 0) {
        return mcp_error(-1, "Missing 'pin' parameter");
    }
    
    // Validate pin range
    if (pin < 0 || pin > 39) {
        return mcp_error(-2, "Invalid pin number (0-39)");
    }
    
    // Get GPIO level
    int32_t level = gpio_get_level(pin);
    if (level < 0) {
        return mcp_error(-3, "Failed to read GPIO level");
    }
    
    // Create response
    snprintf(result_json, sizeof(result_json), 
        "{\"pin\":%d,\"level\":%d,\"state\":\"%s\"}", 
        pin, level, level ? "ON" : "OFF");
    
    return mcp_success(result_json);
}

// Tool: toggle_led - Toggles LED state on specified GPIO pin
mcp_result_t toggle_led_handler(const char* params) {
    int32_t pin;
    char result_json[256];
    
    // Parse pin parameter
    if (mcp_parse_int_param(params, "pin", &pin) != 0) {
        return mcp_error(-1, "Missing 'pin' parameter");
    }
    
    // Validate pin range
    if (pin < 0 || pin > 39) {
        return mcp_error(-2, "Invalid pin number (0-39)");
    }
    
    // Check if pin is protected
    if (gpio_is_pin_protected(pin)) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Pin %d is protected", pin);
        return mcp_error(-4, error_msg);
    }
    
    // Get current state
    int32_t current_level = gpio_get_level(pin);
    if (current_level < 0) {
        return mcp_error(-3, "Failed to read current GPIO level");
    }
    
    // Toggle state
    int32_t new_level = current_level ? 0 : 1;
    
    // Set GPIO direction to output
    if (gpio_set_direction(pin, GPIO_MODE_OUTPUT) != 0) {
        return mcp_error(-5, "Failed to set GPIO direction");
    }
    
    // Set new level
    if (gpio_set_level(pin, new_level) != 0) {
        return mcp_error(-6, "Failed to set GPIO level");
    }
    
    // Create response
    snprintf(result_json, sizeof(result_json), 
        "{\"pin\":%d,\"previous_state\":%d,\"new_state\":%d,\"message\":\"LED toggled %s\"}", 
        pin, current_level, new_level, new_level ? "ON" : "OFF");
    
    // Send SSE event
    char event_data[128];
    snprintf(event_data, sizeof(event_data), "{\"pin\":%d,\"state\":%d}", pin, new_level);
    sse_send_event("gpio_change", event_data);
    
    log_info("LED", result_json);
    return mcp_success(result_json);
}

// Tool: get_system_info - Gets ESP32 system information
mcp_result_t get_system_info_handler(const char* params) {
    char result_json[512];
    
    int32_t free_heap = get_free_heap();
    int32_t uptime = get_uptime_ms();
    int32_t tick_count = get_tick_count();
    
    snprintf(result_json, sizeof(result_json),
        "{\"free_heap\":%d,\"uptime_ms\":%d,\"tick_count\":%d,\"status\":\"running\"}",
        free_heap, uptime, tick_count);
    
    return mcp_success(result_json);
}

// Module initialization - called when WASM module is loaded
void _start() {
    // Initialize MCP server
    mcp_server_init("led_controller", "1.0.0");
    
    // Register tools
    mcp_tool_t tools[] = {
        {
            .name = "control_led",
            .description = "Control LED state on specified GPIO pin. Parameters: pin (number), state (0|1)",
            .handler = control_led_handler
        },
        {
            .name = "get_led_status", 
            .description = "Get current LED state from GPIO pin. Parameters: pin (number)",
            .handler = get_led_status_handler
        },
        {
            .name = "toggle_led",
            .description = "Toggle LED state on specified GPIO pin. Parameters: pin (number)",
            .handler = toggle_led_handler
        },
        {
            .name = "get_system_info",
            .description = "Get ESP32 system information (heap, uptime, etc). No parameters",
            .handler = get_system_info_handler
        }
    };
    
    // Register all tools
    for (int i = 0; i < 4; i++) {
        if (mcp_register_tool(&tools[i]) != 0) {
            log_error("LED", "Failed to register tool");
        }
    }
    
    log_info("LED", "LED Controller MCP module initialized with 4 tools");
}

// Main MCP request handler (required export)
__attribute__((export_name("handle_mcp_request")))
int32_t handle_mcp_request(const char* request, char* response, int32_t max_len) {
    log_info("handle_mcp_request called", request);

    // CRITICAL TEST: Direct string literal access to problematic strings
    const char* test_str1 = "mcp_handle_request:About to check tools/list";
    const char* test_str2 = "mcp_handle_request:Matched tools/list, checking state";
    
    log_info("DIRECT_TEST1", test_str1);
    log_info("DIRECT_TEST2", test_str2);
    log_info("TEST", "Direct string access test completed");

    // Use the full MCP SDK pipeline to handle the request
    return mcp_handle_request(request, response, max_len);
}