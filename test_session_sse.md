# Testing MCP Session-Based SSE Integration with HTTP Header Management

## Overview

The ESP32 MCP Framework now supports complete MCP protocol compliance with session-based Server-Sent Events (SSE) and HTTP header management. This allows:

1. **Per-session event delivery** - SSE events are sent only to the client that triggered them
2. **MCP-Session-Id header support** - Full MCP spec compliance with session headers
3. **Legacy client support** - Older clients can use ?client_id=<uuid> URL parameter
4. **Thread-local session context** - WASM host functions know which session to send events to
5. **HTTP header management** - WASM modules can read request headers and set response headers
6. **Session ID generation** - Server automatically generates session IDs during initialize

## Testing Steps

### 1. Flash and Run the ESP32

```bash
# Build and flash
idf.py build flash

# Monitor logs
idf.py monitor
```

### 2. Get ESP32 IP Address

From the serial monitor logs, find the ESP32 IP address:
```
I (5678) led-wamr: Got IP: 192.168.100.109
```

### 3. Test Session Creation (Initialize Endpoint)

Create a new MCP session - the server will automatically generate a session ID:

```bash
curl -v -X POST http://192.168.100.109/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"initialize","params":{"clientInfo":{"name":"test-client","version":"1.0"}},"id":1}'
```

Expected response with **Mcp-Session-Id header**:
```
< HTTP/1.1 200 OK
< Content-Type: application/json
< Mcp-Session-Id: 12345678-1234-5678-9abc-123456789abc
< Access-Control-Allow-Origin: *
...

{
  "jsonrpc": "2.0",
  "result": {
    "protocolVersion": "2025-06-18",
    "capabilities": {
      "tools": {"listChanged": false},
      "resources": {"subscribe": false, "listChanged": false},
      "prompts": {"listChanged": false},
      "logging": {}
    },
    "serverInfo": {"name": "led_controller", "version": "1.0.0"}
  },
  "id": 1
}
```

**Extract the session ID from the Mcp-Session-Id header for subsequent requests.**

### 4. Test SSE Connection with Session ID

#### Option A: Modern Client (Mcp-Session-Id header)

```bash
curl -N \
  -H "Mcp-Session-Id: 12345678-1234-5678-9abc-123456789abc" \
  http://192.168.100.109/sse
```

#### Option B: Legacy Client (URL parameter)

```bash
curl -N \
  "http://192.168.100.109/sse?client_id=12345678-1234-5678-9abc-123456789abc"
```

Expected SSE output:
```
event: connected
data: SSE connection established

event: ping
data: {"timestamp":123456789}
```

### 5. Test WASM SSE Event Sending

In another terminal, trigger MCP calls that send SSE events:

```bash
# Test LED control with SSE events
curl -X POST http://192.168.100.109/mcp \
  -H "Content-Type: application/json" \
  -H "Mcp-Session-Id: 12345678-1234-5678-9abc-123456789abc" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"control_led","arguments":{"pin":5,"state":1}},"id":2}'
```

The SSE connection should receive events from the WASM module:
```
event: tool_result
data: {"tool":"control_led","result":"success"}

event: led_status
data: {"pin":5,"state":1}
```

### 6. Test Multi-Session Isolation

1. Create two different sessions (different session IDs)
2. Open SSE connections for both sessions  
3. Send MCP requests with different session IDs
4. Verify events are only sent to the correct session

### 7. Test HTTP Header Management

Test WASM module's ability to read request headers:

```bash
# Send request with custom headers
curl -X POST http://192.168.100.109/mcp \
  -H "Content-Type: application/json" \
  -H "Mcp-Session-Id: 12345678-1234-5678-9abc-123456789abc" \
  -H "User-Agent: TestClient/1.0" \
  -H "Authorization: Bearer test-token" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"get_system_info","arguments":{}},"id":3}'
```

The WASM module can now:
- Read headers: `get_request_header("User-Agent")` → "TestClient/1.0"
- Set response headers: `set_response_header("X-Custom", "value")`
- List headers: `list_request_headers()` → "Content-Type,Mcp-Session-Id,User-Agent,Authorization"

### 8. Verify Session Manager Statistics

Check session statistics:
```bash
curl http://192.168.100.109/health
```

Should show active sessions and SSE connections in the logs.

## Expected Behavior

1. **Session Isolation**: Events are only sent to the session that triggered them
2. **Connection Management**: SSE connections are properly registered/unregistered
3. **Thread Safety**: Multiple concurrent sessions work without issues
4. **Automatic Cleanup**: Expired sessions are cleaned up automatically
5. **Error Handling**: Invalid session IDs are handled gracefully

## Architecture Summary

```
1. Initialize Request (Client)
    ↓
HTTP Server (main.c) 
    ↓
mcp_pthread_pool.c (extract headers + set request context)
    ↓
WASM Module: handle_initialize()
    ↓
Host Functions: mcp_generate_session_id() + set_response_header("Mcp-Session-Id", session_id)
    ↓
HTTP Response with Mcp-Session-Id header
    ↓
Client extracts session ID from header

2. Subsequent Requests with Session ID
    ↓
HTTP Server (main.c)
    ↓
mcp_pthread_pool.c (extract session ID + set thread context)
    ↓
WASM Module Execution (with access to get_request_header/set_response_header)
    ↓
Host Function: sse_send_event() (mcp_host_functions.c)
    ↓  
mcp_session_send_to_current() (mcp_session_manager.c)
    ↓
SSE Connection for Current Session
    ↓
Event delivered to specific client
```

## Files Modified

### Core Framework
1. **mcp_session_manager.h/c** - New session management system with thread-local storage
2. **mcp_host_functions.h/c** - Added HTTP header management functions + updated SSE functions
3. **mcp_pthread_pool.h/c** - Enhanced request processing with header extraction/application
4. **mcp_http_server.c** - Added SSE endpoint registration function
5. **main.c** - Initialize session manager and register SSE endpoint
6. **CMakeLists.txt** - Added mcp_session_manager.c to build

### WASM SDK
7. **wasm_sdk/mcp_sdk.h** - Added header management function declarations
8. **wasm_sdk/mcp_sdk.c** - Added session ID generation and header helper functions

### New Host Functions Available to WASM
- `set_response_header(name, value)` - Set HTTP response header
- `get_request_header(name)` - Get HTTP request header value (case-insensitive)
- `remove_response_header(name)` - Remove HTTP response header
- `list_request_headers()` - Get comma-separated list of available request headers
- `mcp_generate_session_id(buffer, size)` - Generate UUID-like session ID
- `mcp_set_session_header(session_id)` - Helper to set Mcp-Session-Id header

## Success Criteria

- ✅ Build compiles without errors or warnings
- ✅ ESP32 boots and connects to WiFi
- ✅ MCP session creation works
- ✅ SSE connections establish with session IDs
- ✅ WASM modules can send SSE events to specific sessions
- ✅ Multiple sessions work independently
- ✅ Legacy client_id parameter support works
- ✅ Session cleanup works properly