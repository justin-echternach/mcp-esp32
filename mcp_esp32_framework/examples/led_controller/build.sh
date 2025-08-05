#!/bin/bash

# Build script for LED controller example
# This script builds the LED controller WASM module using the framework tools

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRAMEWORK_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "Building LED controller example..."

# Use the framework build tool
"$FRAMEWORK_ROOT/tools/build_wasm.sh" \
    -s "$FRAMEWORK_ROOT/wasm_sdk" \
    -o led_controller \
    -v \
    "$SCRIPT_DIR/led_controller.c"

if [ $? -eq 0 ]; then
    echo ""
    echo "Build complete! Output file: wasm/led_controller.was"
    echo ""
    echo "To deploy:"
    echo "1. Copy wasm/led_controller.was to your SD card's /wasm/ directory"
    echo "2. Update your cfg.jsn to map a route to this module"
    echo "3. Flash and run your ESP32 application"
fi