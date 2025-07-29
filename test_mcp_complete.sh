#!/bin/bash
# Complete test script for ESP32 MCP implementation with WASM

ESP32_IP="${1:-192.168.0.xxx}"  # Pass IP as first argument or update default

echo "ESP32 MCP + WASM Test Suite"
echo "==========================="
echo "Testing ESP32 at: $ESP32_IP"
echo ""

# Test 1: Basic connectivity
echo "1. Testing basic HTTP server:"
curl -s http://$ESP32_IP/ | grep -o "ESP32.*Server" || echo "Failed to connect"
echo ""

# Test 2: LED toggle (legacy endpoint)
echo "2. Testing legacy LED toggle:"
curl -X POST http://$ESP32_IP/api/led/toggle
echo -e "\n"

# Test 3: MCP tools/list
echo "3. Testing MCP tools/list:"
curl -X POST http://$ESP32_IP/mcp/led \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/list","id":1}' \
  2>/dev/null | python3 -m json.tool 2>/dev/null || \
  curl -X POST http://$ESP32_IP/mcp/led \
    -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","method":"tools/list","id":1}'
echo ""

# Test 4: MCP ping
echo "4. Testing MCP ping:"
curl -X POST http://$ESP32_IP/mcp/led \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"ping","id":1}'
echo -e "\n"

# Test 5: Control LED via MCP
echo "5. Testing LED control (turn ON):"
curl -X POST http://$ESP32_IP/mcp/led \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"control_led","arguments":"{\"pin\":5,\"state\":1}"},"id":1}'
echo -e "\n"

sleep 2

# Test 6: Get LED status
echo "6. Testing get LED status:"
curl -X POST http://$ESP32_IP/mcp/led \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"get_led_status","arguments":"{\"pin\":5}"},"id":1}'
echo -e "\n"

# Test 7: Toggle LED
echo "7. Testing LED toggle via MCP:"
curl -X POST http://$ESP32_IP/mcp/led \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"toggle_led","arguments":"{\"pin\":5}"},"id":1}'
echo -e "\n"

# Test 8: Get system info
echo "8. Testing system info:"
curl -X POST http://$ESP32_IP/mcp/led \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"get_system_info","arguments":"{}"},"id":1}'
echo -e "\n"

# Test 9: Protected pin (should fail)
echo "9. Testing protected pin access (should fail):"
curl -X POST http://$ESP32_IP/mcp/led \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"control_led","arguments":"{\"pin\":6,\"state\":1}"},"id":1}'
echo -e "\n"

echo "Test complete!"
echo ""
echo "To monitor SSE events in another terminal:"
echo "curl -N http://$ESP32_IP/events"