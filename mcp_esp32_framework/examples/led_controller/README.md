# LED Controller Example

This example demonstrates how to create an MCP WASM module that controls LEDs on an ESP32.

## Features

- Controls LED state (on/off) via MCP protocol
- Reads current LED status
- Toggles LED state
- Provides system information
- Implements proper GPIO protection checks

## Building

From this directory:

```bash
../../tools/build_wasm.sh led_controller.c
```

Or with custom output name:

```bash
../../tools/build_wasm.sh -o my_led_module led_controller.c
```

## MCP Tools Provided

1. **control_led** - Set LED state on specified pin
   - Parameters: `pin` (0-39), `state` (0 or 1)
   
2. **get_led_status** - Read current LED state
   - Parameters: `pin` (0-39)
   
3. **toggle_led** - Toggle LED state
   - Parameters: `pin` (0-39)
   
4. **get_system_info** - Get ESP32 system information
   - No parameters

## Testing

After building and deploying to ESP32:

```bash
# Turn LED on GPIO 5 ON
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"control_led","arguments":{"pin":5,"state":1}},"id":1}'

# Get LED status
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"get_led_status","arguments":{"pin":5}},"id":2}'

# Toggle LED
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"toggle_led","arguments":{"pin":5}},"id":3}'
```

## Configuration

Add to your `cfg.jsn`:

```json
{
  "routes": {
    "/mcp": "led_controller.was"
  }
}
```

## GPIO Protection

The module respects GPIO protection configured in `mcp_config.json`. Protected pins will return an error when accessed.