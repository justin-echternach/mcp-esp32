#!/bin/bash

# Compatibility wrapper for the old build_mcp_wasm.sh script
# This maintains the same behavior as the original hardcoded script

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Call the new parameterized build script with the original parameters
"$SCRIPT_DIR/build_wasm.sh" \
    -w "/home/jechternach/src/esp32/wasi-sdk-20.0" \
    -s "wasm_sdk" \
    -o "led_ctrl" \
    wasm_sdk/example_led_controller.c