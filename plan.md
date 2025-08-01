# MCP LED Controller Implementation Plan

## Current Status ✅
- **PSRAM Integration**: Successfully enabled 4MB PSRAM, eliminated memory constraints
- **Basic MCP Framework**: Working JSON-RPC request parsing and routing
- **WASM Runtime**: WASM modules loading and executing in pthread context
- **SDK Foundation**: Basic MCP SDK structure with tool registration system

## Goal 🎯
Create a complete MCP implementation where an LLM can control ESP32 LEDs through standardized MCP tools/calls.

## Implementation Plan

### Phase 1: Fix Core MCP Request Handling 🔧
**Current Issue**: `handle_mcp_request` only returns parsed method, doesn't handle full MCP flow

**Tasks**:
1. **Update handle_mcp_request function** in `example_led_controller.c`
   - Replace simple method return with full `mcp_handle_request()` call
   - Ensure proper JSON-RPC response format
   - Handle error cases appropriately

2. **Test tools/list method**
   - Verify tools are properly registered and returned
   - Ensure JSON-RPC format compliance
   - Test with curl command

### Phase 2: Complete tools/call Implementation 🛠️
**Tasks**:
1. **Enhance tools/call handling** in `mcp_sdk.c`
   - Fix parameter parsing for nested JSON structures
   - Improve error handling and validation
   - Add proper JSON-RPC response wrapping

2. **Test LED control tools**:
   - `control_led`: Set LED on/off with pin and state parameters
   - `get_led_status`: Read current LED state
   - `toggle_led`: Toggle LED state
   - `get_system_info`: System diagnostics

### Phase 3: JSON-RPC Compliance & Schema 📋
**Based on MCP patterns and JSON-RPC 2.0**:

**tools/list Request**:
```json
{
  "jsonrpc": "2.0",
  "method": "tools/list",
  "id": 1
}
```

**tools/list Response**:
```json
{
  "jsonrpc": "2.0",
  "result": {
    "tools": [
      {
        "name": "control_led",
        "description": "Control LED state on specified GPIO pin",
        "inputSchema": {
          "type": "object",
          "properties": {
            "pin": {"type": "number", "description": "GPIO pin number (0-39)"},
            "state": {"type": "number", "description": "LED state (0=off, 1=on)"}
          },
          "required": ["pin", "state"]
        }
      }
    ]
  },
  "id": 1
}
```

**tools/call Request**:
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "control_led",
    "arguments": {
      "pin": 2,
      "state": 1
    }
  },
  "id": 2
}
```

**tools/call Response**:
```json
{
  "jsonrpc": "2.0",
  "result": {
    "content": [
      {
        "type": "text",
        "text": "LED on pin 2 turned ON successfully"
      }
    ]
  },
  "id": 2
}
```

### Phase 4: Host Function Integration 🔗
**Tasks**:
1. **Verify host function imports** are working:
   - `gpio_set_level`, `gpio_get_level`, `gpio_set_direction`
   - `json_parse_request`, `json_create_response`, `json_create_error`
   - `log_info`, `log_error`

2. **Add missing host functions** if needed:
   - Pin protection checking
   - System info functions
   - SSE event sending

### Phase 5: LLM Integration Testing 🤖
**Test Scenarios**:
1. **Direct curl testing**:
   ```bash
   # List available tools
   curl -X POST http://192.168.100.109/mcp/led \
     -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","method":"tools/list","id":1}'

   # Turn LED on
   curl -X POST http://192.168.100.109/mcp/led \
     -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"control_led","arguments":{"pin":2,"state":1}},"id":2}'
   ```

2. **MCP Client Integration**:
   - Test with Claude Desktop or other MCP-compatible clients
   - Verify tool discovery and execution
   - Test error handling and edge cases

### Phase 6: Production Readiness 🚀
**Tasks**:
1. **Error Handling & Validation**:
   - GPIO pin range validation (0-39)
   - Pin protection checks
   - State validation (0/1)
   - JSON parsing error recovery

2. **Performance & Reliability**:
   - Memory usage optimization
   - Response time measurement
   - Concurrent request handling
   - WASM module lifecycle management

3. **Extended Tools**:
   - PWM control for brightness
   - Multiple LED patterns
   - GPIO input reading
   - System monitoring tools

## Current Code Issues to Fix

### 1. handle_mcp_request Function
**File**: `wasm_sdk/example_led_controller.c:201`
**Issue**: Only returns parsed method, should handle full MCP request
**Fix**: Replace with call to `mcp_handle_request()`

### 2. JSON Response Format  
**Issue**: Not following JSON-RPC 2.0 format
**Fix**: Update `json_create_response()` and `json_create_error()` functions

### 3. Parameter Parsing
**Issue**: Nested JSON parameter parsing incomplete
**Fix**: Enhance `mcp_parse_string_param()` for complex JSON structures

### 4. Tool Schema
**Issue**: Missing `inputSchema` in tools/list response
**Fix**: Add schema definitions to tool registration

## Success Criteria ✨
- [ ] LLM can discover available LED tools via tools/list
- [ ] LLM can turn LED on/off via tools/call
- [ ] All responses follow JSON-RPC 2.0 format
- [ ] Error handling works for invalid requests
- [ ] Multiple tools work concurrently
- [ ] Real-time LED control from LLM commands

## String Handling Documentation
For comprehensive information about WASM string literals and host function string conversion, see:
**[WAMR ESP32 String Handling Guide](./wamr-esp32-string-handling.md)**

This document covers:
- String literal corruption issues and solutions
- Host function signature patterns (`$`, `*`, `~`)
- Function name collision problems (e.g., `strcmp` → `wasm_strcmp`)
- Memory layout configuration for WAMR
- String handling best practices and debugging

## Next Immediate Steps
1. Apply WAMR build configuration fixes (memory layout exports)
2. Test complete MCP flow with working string handling
3. Implement tools/call with control_led functionality
4. Verify LED actually turns on/off
5. Create comprehensive test suite