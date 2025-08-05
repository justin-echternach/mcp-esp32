#!/bin/bash

# Build script for MCP WASM modules using WASI SDK
# This is a parameterized version that can be used in any project

set -e

# Script name for help messages
SCRIPT_NAME=$(basename "$0")

# Default values
DEFAULT_OUTPUT_DIR="wasm"
DEFAULT_SDK_RELATIVE="../wasm_sdk"
DEFAULT_OUTPUT_NAME=""
DEFAULT_EXPORTS="handle_mcp_request"
DEFAULT_STACK_SIZE="65536"
DEFAULT_INITIAL_MEMORY="131072"

# Function to display help
show_help() {
    cat << EOF
Usage: $SCRIPT_NAME [OPTIONS] <source_files...>

Build MCP WASM modules using WASI SDK with ESP32-compatible settings.

OPTIONS:
    -h, --help              Show this help message
    -o, --output NAME       Output filename (without .was extension)
                           If not specified, uses first source file name
    -d, --output-dir DIR    Output directory (default: $DEFAULT_OUTPUT_DIR)
    -s, --sdk-path PATH     Path to MCP SDK directory (default: $DEFAULT_SDK_RELATIVE)
    -w, --wasi-sdk PATH     Path to WASI SDK installation
                           Can also be set via WASI_SDK_PATH environment variable
    -e, --export FUNC       Additional function to export (can be used multiple times)
                           Default exports: $DEFAULT_EXPORTS
    --stack-size SIZE       Stack size in bytes (default: $DEFAULT_STACK_SIZE)
    --memory SIZE          Initial memory size in bytes (default: $DEFAULT_INITIAL_MEMORY)
    -v, --verbose          Enable verbose output

EXAMPLES:
    # Build with automatic SDK detection
    $SCRIPT_NAME my_module.c

    # Build with custom output name
    $SCRIPT_NAME -o led_controller my_led_module.c

    # Build with custom WASI SDK path
    $SCRIPT_NAME -w /opt/wasi-sdk-20.0 my_module.c

    # Build with additional exports
    $SCRIPT_NAME -e my_init_function -e my_cleanup_function my_module.c

    # Build multiple source files
    $SCRIPT_NAME my_module.c helper_functions.c

ENVIRONMENT VARIABLES:
    WASI_SDK_PATH          Path to WASI SDK installation

EOF
}

# Parse command line arguments
VERBOSE=0
OUTPUT_NAME=""
OUTPUT_DIR="$DEFAULT_OUTPUT_DIR"
SDK_PATH="$DEFAULT_SDK_RELATIVE"
WASI_SDK=""
EXPORTS=()
STACK_SIZE="$DEFAULT_STACK_SIZE"
INITIAL_MEMORY="$DEFAULT_INITIAL_MEMORY"
SOURCE_FILES=()

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -o|--output)
            OUTPUT_NAME="$2"
            shift 2
            ;;
        -d|--output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -s|--sdk-path)
            SDK_PATH="$2"
            shift 2
            ;;
        -w|--wasi-sdk)
            WASI_SDK="$2"
            shift 2
            ;;
        -e|--export)
            EXPORTS+=("$2")
            shift 2
            ;;
        --stack-size)
            STACK_SIZE="$2"
            shift 2
            ;;
        --memory)
            INITIAL_MEMORY="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -*)
            echo "Error: Unknown option $1"
            echo "Use $SCRIPT_NAME --help for usage information"
            exit 1
            ;;
        *)
            SOURCE_FILES+=("$1")
            shift
            ;;
    esac
done

# Check if source files were provided
if [ ${#SOURCE_FILES[@]} -eq 0 ]; then
    echo "Error: No source files specified"
    echo "Use $SCRIPT_NAME --help for usage information"
    exit 1
fi

# Determine WASI SDK path
if [ -z "$WASI_SDK" ]; then
    if [ -n "$WASI_SDK_PATH" ]; then
        WASI_SDK="$WASI_SDK_PATH"
    else
        # Try to find WASI SDK in common locations
        for path in "/opt/wasi-sdk-20.0" "/usr/local/wasi-sdk" "$HOME/wasi-sdk-20.0" "$HOME/src/esp32/wasi-sdk-20.0"; do
            if [ -d "$path" ] && [ -f "$path/bin/clang" ]; then
                WASI_SDK="$path"
                echo "Found WASI SDK at: $WASI_SDK"
                break
            fi
        done
    fi
fi

# Check if WASI SDK is available
if [ -z "$WASI_SDK" ] || ! command -v "$WASI_SDK/bin/clang" &> /dev/null; then
    echo "Error: WASI SDK not found"
    echo "Please specify the path using -w option or set WASI_SDK_PATH environment variable"
    echo "You can download WASI SDK from: https://github.com/WebAssembly/wasi-sdk/releases"
    exit 1
fi

# Determine output name if not specified
if [ -z "$OUTPUT_NAME" ]; then
    # Use first source file name without extension
    OUTPUT_NAME=$(basename "${SOURCE_FILES[0]}" | sed 's/\.[^.]*$//')
fi

# Create output directory if it doesn't exist
mkdir -p "$OUTPUT_DIR"

# Build export flags
EXPORT_FLAGS="-Wl,--export=$DEFAULT_EXPORTS"
for export in "${EXPORTS[@]}"; do
    EXPORT_FLAGS="$EXPORT_FLAGS -Wl,--export=$export"
done

# Add default ESP32 WASM exports
EXPORT_FLAGS="$EXPORT_FLAGS -Wl,--export=_start -Wl,--export=__data_end -Wl,--export=__heap_base"

# Find MCP SDK files
SDK_SOURCES=()
if [ -f "$SDK_PATH/mcp_sdk.c" ]; then
    SDK_SOURCES+=("$SDK_PATH/mcp_sdk.c")
    echo "Including MCP SDK from: $SDK_PATH"
else
    echo "Warning: MCP SDK not found at $SDK_PATH/mcp_sdk.c"
    echo "Building without SDK (make sure your module includes necessary functions)"
fi

# Prepare include path for SDK headers
INCLUDE_FLAGS=""
if [ -d "$SDK_PATH" ]; then
    INCLUDE_FLAGS="-I$SDK_PATH"
fi

# Build command
OUTPUT_FILE="$OUTPUT_DIR/${OUTPUT_NAME}.was"

echo "Building MCP WASM module: ${OUTPUT_NAME}"
echo "Source files: ${SOURCE_FILES[*]} ${SDK_SOURCES[*]}"
echo "Output: $OUTPUT_FILE"

if [ $VERBOSE -eq 1 ]; then
    echo "WASI SDK: $WASI_SDK"
    echo "Stack size: $STACK_SIZE bytes"
    echo "Initial memory: $INITIAL_MEMORY bytes"
    echo "Exports: $DEFAULT_EXPORTS ${EXPORTS[*]}"
fi

# Compile C to WASM with ESP32-compatible memory settings
"$WASI_SDK/bin/clang" \
    --target=wasm32-wasi \
    -O3 \
    $INCLUDE_FLAGS \
    -z stack-size=$STACK_SIZE \
    -Wl,--initial-memory=$INITIAL_MEMORY \
    -nostdlib \
    -Wl,--no-entry \
    $EXPORT_FLAGS \
    -Wl,--global-base=4096 \
    -Wl,--strip-all \
    -Wl,--allow-undefined \
    -o "$OUTPUT_FILE" \
    "${SOURCE_FILES[@]}" \
    "${SDK_SOURCES[@]}"

# Check if compilation succeeded
if [ -f "$OUTPUT_FILE" ]; then
    FILE_SIZE=$(wc -c < "$OUTPUT_FILE")
    echo "✅ Successfully built $OUTPUT_FILE"
    echo "📊 File size: $FILE_SIZE bytes"
    
    # Memory usage estimation
    TOTAL_MEMORY=$((INITIAL_MEMORY / 1024))
    STACK_KB=$((STACK_SIZE / 1024))
    echo "💾 Memory: ${TOTAL_MEMORY}KB total (${STACK_KB}KB stack)"
    
    echo ""
    echo "📋 Next steps:"
    echo "1. Copy $OUTPUT_FILE to SD card /wasm/ directory"
    echo "2. Configure your MCP routes in cfg.jsn"
    echo "3. Flash ESP32 with your application"
    echo "4. Test MCP endpoints"
else
    echo "❌ Build failed"
    exit 1
fi