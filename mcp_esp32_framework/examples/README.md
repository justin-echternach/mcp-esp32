# MCP Framework Examples

This directory contains example WASM modules demonstrating how to use the ESP32 MCP Framework.

## Available Examples

### 1. Minimal Example (`minimal/`)
The simplest possible MCP module showing basic structure and concepts.

**Features:**
- Simple greeting tool with optional parameter
- Echo tool that returns input
- Basic math (addition) tool
- Demonstrates tool registration and parameter handling

**Use this example to:**
- Learn MCP SDK basics  
- Understand tool registration
- See parameter parsing examples
- Get a working starting point

### 2. LED Controller Example (`led_controller/`)
Complete hardware control example for ESP32 GPIO and LEDs.

**Features:**
- LED control (on/off/toggle) with GPIO protection
- LED status reading
- System information reporting
- SSE event generation
- GPIO safety checks

**Use this example to:**
- Control ESP32 hardware from WASM
- Understand GPIO protection system
- See real-time event generation
- Learn hardware abstraction patterns

## Quick Start

1. **Choose an example** based on your needs
2. **Build the WASM module**:
   ```bash
   cd minimal/  # or led_controller/
   ./build.sh
   ```
3. **Deploy to ESP32**:
   - Copy `wasm/*.was` file to SD card `/wasm/` directory
   - Update your `cfg.jsn` configuration
   - Flash and run your ESP32 application

## Building Examples

Each example includes its own build script, but you can also build manually:

```bash
# From any example directory
../../tools/build_wasm.sh [options] source_file.c

# Example with custom output
../../tools/build_wasm.sh -o my_module my_source.c
```

## Configuration

Add your WASM modules to the ESP32 configuration:

**cfg.jsn:**
```json
{
  "routes": {
    "/mcp": "minimal_mcp.was",
    "/mcp/led": "led_controller.was"
  }
}
```

## Testing Examples

All examples respond to MCP JSON-RPC 2.0 protocol:

```bash
# List available tools
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/list","id":1}'

# Call a specific tool
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"tool_name","arguments":{}},"id":2}'
```

## Creating Your Own Module

1. **Start with minimal example** as a template
2. **Define your tool handlers** - functions that process MCP requests
3. **Register tools in `_start()`** - initialize your module
4. **Implement `handle_mcp_request()`** - required entry point
5. **Build and test** using the framework tools

## Common Patterns

### Tool Handler Structure
```c
mcp_result_t my_tool_handler(const char* params) {
    // Parse parameters
    int32_t value;
    if (mcp_parse_int_param(params, "value", &value) != 0) {
        return mcp_error(-1, "Missing 'value' parameter");
    }
    
    // Do work
    char response[256];
    snprintf(response, sizeof(response), "Result: %d", value * 2);
    
    // Return success
    return mcp_success(response);
}
```

### Tool Registration
```c
void _start() {
    mcp_server_init("my_module", "1.0.0");
    
    mcp_tool_t tools[1];
    strcpy(tools[0].name, "my_tool");
    strcpy(tools[0].description, "My tool description");
    tools[0].handler = my_tool_handler;
    
    mcp_register_tool(&tools[0]);
}
```

## Next Steps

- Explore the framework host code in `../host/`
- Read the WASM SDK documentation in `../wasm_sdk/`
- Check out the build tools in `../tools/`
- See the main project README for complete setup instructions