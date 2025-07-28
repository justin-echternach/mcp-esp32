#!/bin/bash

# Build Rust WASM for LED control with ESP32-compatible settings
echo "Building Rust WASM with ESP32-compatible settings..."

cd wasm-led

# Add wasm32 target if not already added
rustup target add wasm32-unknown-unknown 2>/dev/null

# Set target and build flags to exactly match working ESP32 example
export RUSTFLAGS="-C link-arg=-zstack-size=4096 -C link-arg=--initial-memory=65536 -C link-arg=--export=__data_end -C link-arg=--export=__heap_base -C link-arg=--strip-all -C link-arg=--no-entry -C link-arg=--allow-undefined"

# Build the WASM module with optimization
cargo build --target wasm32-unknown-unknown --release

# Check if build succeeded
if [ $? -eq 0 ]; then
    echo "Build successful!"
    
    # Use wasm-opt if available to further optimize
    if command -v wasm-opt &> /dev/null; then
        echo "Optimizing WASM with wasm-opt..."
        wasm-opt -O3 --enable-bulk-memory target/wasm32-unknown-unknown/release/led_controller.wasm -o target/wasm32-unknown-unknown/release/led_controller_opt.wasm
        cp target/wasm32-unknown-unknown/release/led_controller_opt.wasm ../led_controller.wasm
    else
        echo "wasm-opt not found, using unoptimized WASM"
        cp target/wasm32-unknown-unknown/release/led_controller.wasm ../led_controller.wasm
    fi
    
    # Generate C header file
    cd ..
    echo "Generating C header..."
    xxd -i led_controller.wasm > led_controller_wasm.h
    
    # Fix the variable names in the header
    sed -i 's/unsigned char led_controller_wasm\[\]/static const uint8_t led_controller_wasm[]/' led_controller_wasm.h
    sed -i 's/unsigned int led_controller_wasm_len/static const size_t led_controller_wasm_len/' led_controller_wasm.h
    
    echo "WASM build complete!"
    echo "Generated files:"
    echo "  - led_controller.wasm ($(wc -c < led_controller.wasm) bytes)"
    echo "  - led_controller_wasm.h"
    
    # Show WASM size comparison
    echo "Size comparison:"
    echo "  Manual C WASM: 127 bytes"
    echo "  Rust WASM: $(wc -c < led_controller.wasm) bytes"
else
    echo "Build failed!"
    exit 1
fi