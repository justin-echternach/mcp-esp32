#!/bin/bash
# Quick MCP test for running ESP32

ESP32_IP="192.168.100.109"

echo "Testing MCP on ESP32 at $ESP32_IP"
echo ""

# Test basic connectivity
echo "1. Testing basic HTTP:"
curl -s http://$ESP32_IP/ | head -3
echo ""

# Test if MCP routes are registered (will fail if routes not parsed)
echo "2. Testing MCP route (may fail if config didn't parse):"
curl -X POST http://$ESP32_IP/mcp/led \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"ping","id":1}' \
  2>&1
echo -e "\n"

# Test legacy LED toggle (should work)
echo "3. Testing legacy LED toggle (should work):"
curl -X POST http://$ESP32_IP/api/led/toggle
echo -e "\n"

# Try with the config we have - direct routes
echo "4. Trying direct routes that might be hardcoded:"
for route in /mcp /mcp/led /mcp/gpio /mcp/system; do
  echo "Testing $route:"
  curl -X POST http://$ESP32_IP$route \
    -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","method":"tools/list","id":1}' \
    2>&1 | head -1
done