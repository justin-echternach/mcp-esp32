#!/bin/bash

# Build script for MCP LED control C WASM using WASI SDK
# Uses same parameters as working minimal example but exports MCP functions

set -e

echo "Building MCP LED control C WASM example with working memory settings..."

# Set WASI SDK path
WASI_SDK_PATH="/home/jechternach/src/esp32/wasi-sdk-20.0"

# Check if WASI SDK is available
if ! command -v "$WASI_SDK_PATH/bin/clang" &> /dev/null; then
    echo "Error: WASI SDK not found at $WASI_SDK_PATH"
    echo "Please install WASI SDK or adjust the path"
    exit 1
fi

# Compile C to WASM with ESP32-compatible memory settings (like working led.c)
"$WASI_SDK_PATH/bin/clang" \
    --target=wasm32-wasi \
    -O3 \
    -z stack-size=16384 \
    -Wl,--initial-memory=131072 \
    -nostdlib \
    -Wl,--no-entry \
    -Wl,--export=handle_mcp_request \
    -Wl,--export=_start \
    -Wl,--strip-all \
    -Wl,--allow-undefined \
    -o wasm/led_ctrl.was \
    wasm_sdk/example_led_controller.c \
    wasm_sdk/mcp_sdk.c

# Check if compilation succeeded
if [ -f wasm/led_ctrl.was ]; then
    echo "✅ Successfully built led_ctrl.was"
    echo "📊 File size: $(wc -c < wasm/led_ctrl.was) bytes"
    
    echo "✅ Generated wasm/led_ctrl.was"
    echo "🔗 Ready to copy to SD card /wasm/ directory"
    echo ""
    echo "📋 Next steps:"
    echo "1. Copy led_ctrl.was to SD card /wasm/ directory"
    echo "2. Copy cfg.jsn to SD card root"
    echo "3. Flash ESP32 with: idf.py flash"
    echo "4. Test with: ./test_mcp_now.sh"
else
    echo "❌ Build failed"
    exit 1
fi