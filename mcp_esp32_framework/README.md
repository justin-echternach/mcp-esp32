# ESP32 MCP Framework

A comprehensive WebAssembly-based Model Context Protocol (MCP) framework for ESP32 microcontrollers.

## Overview

This framework enables LLMs to directly control ESP32 hardware through MCP JSON-RPC 2.0 protocol, with core logic running in sandboxed WebAssembly (WASM) modules.

## Architecture

```
LLM Client (Claude/ChatGPT)
    ↓ HTTP/JSON-RPC 2.0
ESP32 HTTP Server (host framework)
    ↓ Function calls
WASM Runtime (WAMR)
    ↓ Host functions
WASM Module (mcp_sdk)
    ↓ GPIO calls
ESP32 Hardware (LEDs/GPIO)
```

## Structure

```
mcp_esp32_framework/
├── host/                    # ESP32 host-side framework
│   ├── include/            # Framework headers
│   └── src/                # Framework implementation
├── wasm_sdk/               # WASM module development SDK
│   ├── mcp_sdk.h          # SDK header for WASM modules
│   ├── mcp_sdk.c          # SDK implementation
│   └── Makefile           # Build configuration
├── tools/                  # Build and development tools
│   └── build_mcp_wasm.sh  # WASM compilation script
├── examples/               # Example projects and modules
└── docs/                  # Documentation
```

## Key Features

- **PSRAM Support**: Enables 4MB memory for complex WASM operations
- **Thread Pool**: Pthread-based request handling for concurrency
- **GPIO Protection**: Configurable pin protection system
- **JSON-RPC 2.0**: Full MCP protocol implementation
- **Host Functions**: Comprehensive ESP32 API for WASM modules
- **Hot Loading**: Dynamic WASM module loading from SD card
- **SSE Events**: Real-time event streaming support

## Hardware Requirements

- **ESP32-WROVER-E** (with PSRAM) - 4MB memory required
- **SD card** (optional) - for WASM modules and configuration
- **GPIO pins** - for hardware control

## Quick Start

See individual documentation files for:
- [Host Framework Integration](docs/host_integration.md)
- [WASM Module Development](docs/wasm_development.md)
- [Build System Setup](docs/build_setup.md)

## Version

Current version extracted from led-test project.
Built with ESP-IDF v5.5 and WAMR v2.4.0.

---

*This framework demonstrates working MCP-over-WASM architecture for ESP32 hardware control with extensive debugging and optimization.*