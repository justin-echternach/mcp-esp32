# Minimal MCP Module Example

This is the simplest possible MCP module demonstrating the basic structure.

## Features

- Simple "hello" tool with optional name parameter
- Echo tool that returns input message
- Minimal code showing MCP SDK usage

## Building

```bash
../../tools/build_wasm.sh minimal_mcp.c
```

## Testing

After deploying to ESP32:

```bash
# Say hello
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"hello","arguments":{}},"id":1}'

# Say hello with name
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"hello","arguments":{"name":"ESP32"}},"id":2}'

# Echo a message
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"echo","arguments":{"message":"Hello from WASM!"}},"id":3}'

# Add two numbers
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"add","arguments":{"a":15,"b":27}},"id":4}'

# List available tools
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/list","id":5}'
```

## Key Concepts

1. **Include MCP SDK**: `#include "mcp_sdk.h"`
2. **Define tool handlers**: Functions that process parameters and return results
3. **Register tools**: Array of `mcp_tool_t` structures
4. **Implement `handle_mcp_request`**: Required entry point

## Next Steps

- Add more tools to your module
- Use host functions (GPIO, system info, etc.)
- Handle errors and edge cases
- See the LED controller example for hardware control