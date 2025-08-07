#!/bin/bash

# Build script for Music Player example
# This script builds the music player WASM module using the framework tools

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRAMEWORK_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "Building Music Player example..."

# Use the framework build tool
"$FRAMEWORK_ROOT/tools/build_wasm.sh" \
    -s "$FRAMEWORK_ROOT/wasm_sdk" \
    -o music_player \
    -v \
    "$SCRIPT_DIR/music_player.c"

if [ $? -eq 0 ]; then
    echo ""
    echo "Build complete! Output file: wasm/music_player.was"
    echo ""
    echo "To deploy:"
    echo "1. Copy wasm/music_player.was to your SD card's /wasm/ directory"
    echo "2. Update your cfg.jsn to map a route to this module"
    echo "3. Flash and run your ESP32 application"
    echo ""
    echo "Hardware setup:"
    echo "- Connect speaker driver signal input to ESP32 GPIO 25"
    echo "- Connect driver power (VCC/GND) to ESP32 power rails"
    echo "- Connect speaker to driver outputs"
fi