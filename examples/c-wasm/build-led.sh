#!/bin/bash

# Build script for LED control C WASM using WASI SDK
# Uses same parameters as working minimal example

set -e

echo "Building LED control C WASM example with working memory settings..."

# Set WASI SDK path
WASI_SDK_PATH="/home/jechternach/src/esp32/wasi-sdk-20.0"

# Check if WASI SDK is available
if ! command -v "$WASI_SDK_PATH/bin/clang" &> /dev/null; then
    echo "Error: WASI SDK not found at $WASI_SDK_PATH"
    echo "Please install WASI SDK or adjust the path"
    exit 1
fi

# Compile C to WASM with ESP32-compatible memory settings (exactly like minimal.wasm)
"$WASI_SDK_PATH/bin/clang" \
    --target=wasm32-wasi \
    -O3 \
    -z stack-size=4096 \
    -Wl,--initial-memory=65536 \
    -nostdlib \
    -Wl,--no-entry \
    -Wl,--export=led_blink_task \
    -Wl,--export=led_on \
    -Wl,--export=led_off \
    -Wl,--export=led_toggle \
    -Wl,--strip-all \
    -Wl,--allow-undefined \
    -o led.wasm \
    led.c

# Check if compilation succeeded
if [ -f led.wasm ]; then
    echo "✅ Successfully built led.wasm"
    echo "📊 File size: $(wc -c < led.wasm) bytes"
    
    # Generate C header file
    echo "Generating C header..."
    xxd -i led.wasm > led_wasm.h
    
    # Update variable names to match our convention
    sed -i 's/led_wasm/led_c_wasm/g' led_wasm.h
    
    echo "✅ Generated led_wasm.h"
    echo "🔗 Ready to include in ESP32 project"
else
    echo "❌ Build failed"
    exit 1
fi