#!/bin/bash

# Build script for minimal MCP example

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRAMEWORK_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "Building minimal MCP example..."

# Use the framework build tool
"$FRAMEWORK_ROOT/tools/build_wasm.sh" \
    -s "$FRAMEWORK_ROOT/wasm_sdk" \
    -o minimal_mcp \
    -e _start \
    "$SCRIPT_DIR/minimal_mcp.c"