#!/bin/bash
# Test script for ESP32 MCP implementation

ESP32_IP="192.168.0.xxx"  # Update with your ESP32 IP

echo "Testing MCP implementation on ESP32 at $ESP32_IP"

# Test basic connectivity
echo -e "\n1. Testing basic HTTP:"
curl -s http://$ESP32_IP/ | head -5

# Test SSE events
echo -e "\n2. Testing SSE (press Ctrl+C after seeing events):"
timeout 5 curl -N http://$ESP32_IP/events || true

# Test LED toggle (old endpoint)  
echo -e "\n3. Testing LED toggle:"
curl -X POST http://$ESP32_IP/api/led/toggle

# Test MCP endpoint (will fail without WASM, but shows routing works)
echo -e "\n4. Testing MCP routing:"
curl -X POST http://$ESP32_IP/mcp/led \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"ping","id":1}'

echo -e "\nTest complete!"