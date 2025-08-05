# Framework Structure Overview

## Directory Layout

### `host/` - ESP32 Host-Side Framework
Contains the ESP32 application framework that runs the HTTP server, WAMR runtime, and provides host functions to WASM modules.

**Key Components:**
- `mcp_config.h` - Configuration structures and constants
- `mcp_config_loader.h/c` - Smart configuration loading with fallbacks
- `mcp_host_functions.h/c` - Host functions exported to WASM modules
- `mcp_http_server.h/c` - HTTP server with MCP endpoint registration
- `mcp_pthread_pool.h/c` - Thread pool for handling MCP requests
- `mcp_wasm_runtime.h/c` - WASM module management and execution
- `mcp_wifi_minimal.h/c` - Minimal WiFi management functions

### `wasm_sdk/` - WASM Module Development SDK
SDK for developing WebAssembly modules that implement MCP servers.

**Key Components:**
- `mcp_sdk.h` - API definitions for WASM modules
- `mcp_sdk.c` - SDK implementation with MCP protocol helpers
- `Makefile` - Build configuration for WASM modules

### `tools/` - Build and Development Tools
Scripts and utilities for building and managing the framework.

**Key Components:**
- `build_mcp_wasm.sh` - WASM compilation script using WASI-SDK

### `examples/` - Example Projects and Modules
Complete example projects showing framework usage.

**Planned Components:**
- `led_controller/` - Complete LED control example
- `minimal/` - Minimal starter project template

### `docs/` - Documentation
Comprehensive documentation for framework usage.

**Components:**
- `structure.md` - This file
- `host_integration.md` - How to integrate the host framework
- `wasm_development.md` - How to develop WASM modules
- `build_setup.md` - Build system configuration

## Integration Model

The framework follows a clear separation between host and WASM components:

1. **Host Framework** provides infrastructure (HTTP server, WASM runtime, hardware access)
2. **WASM SDK** provides API for implementing MCP servers in WebAssembly
3. **Build Tools** compile WASM modules with correct memory settings
4. **Examples** demonstrate complete integration

This allows developers to focus on MCP business logic while the framework handles all ESP32-specific concerns.