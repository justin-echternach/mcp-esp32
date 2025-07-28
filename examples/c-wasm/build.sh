#!/bin/bash

# Build script for C WASM using WASI SDK
# This creates a simple WASM module for testing WAMR execution environment

set -e

echo "Building C WASM example..."

# Set WASI SDK path
WASI_SDK_PATH="/home/jechternach/src/esp32/wasi-sdk-20.0"

# Check if WASI SDK is available
if ! command -v "$WASI_SDK_PATH/bin/clang" &> /dev/null; then
    echo "Error: WASI SDK not found at $WASI_SDK_PATH"
    echo "Please install WASI SDK or adjust the path"
    exit 1
fi

# Compile C to WASM
"$WASI_SDK_PATH/bin/clang" \
    --target=wasm32-wasi \
    -nostdlib \
    -Wl,--no-entry \
    -Wl,--export-all \
    -O2 \
    -o simple.wasm \
    simple.c

# Check if compilation succeeded
if [ -f simple.wasm ]; then
    echo "✅ Successfully built simple.wasm"
    echo "📊 File size: $(wc -c < simple.wasm) bytes"
    
    # Generate C header file
    echo "Generating C header..."
    xxd -i simple.wasm > simple_wasm.h
    
    # Update variable names to match our convention
    sed -i 's/simple_wasm/simple_c_wasm/g' simple_wasm.h
    
    echo "✅ Generated simple_wasm.h"
    echo "🔗 Ready to include in ESP32 project"
else
    echo "❌ Build failed"
    exit 1
fi