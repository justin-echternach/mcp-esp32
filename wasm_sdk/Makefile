# Makefile for building MCP WASM modules for ESP32
# Requires WASI SDK: https://github.com/WebAssembly/wasi-sdk

# WASI SDK path - adjust as needed
WASI_SDK_PATH ?= /home/jechternach/src/esp32/wasi-sdk-20.0

# Compiler settings
CC = $(WASI_SDK_PATH)/bin/clang
CFLAGS = -O2 -Wall -Wextra -std=c11 \
         -Wl,--no-entry \
         -Wl,--export=handle_mcp_request \
         -Wl,--export=_start \
         -Wl,--allow-undefined \
         -nostdlib

# Source files
SDK_SOURCES = mcp_sdk.c
EXAMPLE_SOURCES = example_led_controller.c

# Output directory
OUTPUT_DIR = ../wasm

# Default target
all: $(OUTPUT_DIR)/led_controller.wasm

# Create output directory
$(OUTPUT_DIR):
	mkdir -p $(OUTPUT_DIR)

# Build LED controller example
$(OUTPUT_DIR)/led_controller.wasm: $(EXAMPLE_SOURCES) $(SDK_SOURCES) mcp_sdk.h | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ $(EXAMPLE_SOURCES) $(SDK_SOURCES)
	@echo "Built: $@"

# Build custom WASM module
%.wasm: %.c $(SDK_SOURCES) mcp_sdk.h | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $(OUTPUT_DIR)/$@ $< $(SDK_SOURCES)
	@echo "Built: $(OUTPUT_DIR)/$@"

# Clean build artifacts
clean:
	rm -f $(OUTPUT_DIR)/*.wasm

# Help
help:
	@echo "ESP32 MCP WASM SDK Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all                    - Build LED controller example"
	@echo "  led_controller.wasm    - Build LED controller example"
	@echo "  your_module.wasm       - Build custom module from your_module.c"
	@echo "  clean                  - Remove all built WASM files"
	@echo "  help                   - Show this help"
	@echo ""
	@echo "Requirements:"
	@echo "  - WASI SDK installed at $(WASI_SDK_PATH)"
	@echo "  - Download from: https://github.com/WebAssembly/wasi-sdk/releases"
	@echo ""
	@echo "Example usage:"
	@echo "  make led_controller.wasm"
	@echo "  make my_sensor.wasm      # builds from my_sensor.c"
	@echo "  WASI_SDK_PATH=/usr/local/wasi-sdk make all"

.PHONY: all clean help