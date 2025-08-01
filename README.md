# ESP32 MCP WASM LED Controller

This project implements a **Model Context Protocol (MCP) server** using **WebAssembly (WASM)** modules for LED control on ESP32. It enables LLMs to directly control ESP32 hardware through MCP JSON-RPC 2.0 protocol, with core logic running in sandboxed WASM modules.

## 🎯 Key Achievement
Successfully enabled **4MB PSRAM** on ESP32-WROVER-E, expanding from ~180KB to 4MB available memory for WASM operations.

## 🏗️ Architecture
```
LLM Client (Claude/ChatGPT)
    ↓ HTTP/JSON-RPC 2.0
ESP32 HTTP Server (main.c)
    ↓ Function calls
WASM Runtime (WAMR)
    ↓ Host functions
WASM Module (led_ctrl.was)
    ↓ GPIO calls
ESP32 Hardware (LEDs/GPIO)
```

## 🚀 Quick Start

### Prerequisites
- **ESP32-WROVER-E** (with PSRAM)
- SD card formatted FAT32
- ESP-IDF v5.5
- WASI-SDK 20.0

### Build and Flash
```bash
# Set up environment
. $HOME/esp/v5.5/esp-idf/export.sh

# Build WASM module
./build_mcp_wasm.sh

# Build ESP32 firmware
idf.py build

# Flash to device
idf.py flash

# Monitor logs
idf.py monitor
```

### Test MCP Endpoints
```bash
# Test tools/list
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/list","id":1}'

# Test LED control
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"control_led","arguments":{"pin":5,"state":1}},"id":2}'
```

## 📁 Project Structure

```
├── main/
│   ├── main.c                     # ESP32 main application + WAMR integration
│   └── mcp_pthread_handler.c      # MCP HTTP endpoint handlers
├── wasm_sdk/
│   ├── mcp_sdk.h                  # WASM SDK header with MCP interfaces
│   ├── mcp_sdk.c                  # WASM SDK implementation
│   └── example_led_controller.c   # Example LED controller WASM module
├── build_mcp_wasm.sh              # WASM build script (WASI-SDK)
├── CMakeLists.txt                 # ESP32 build config (includes PSRAM)
├── sdkconfig.defaults             # ESP32 configuration with PSRAM settings
├── cfg.jsn                        # WASM module configuration
└── mcp_config.json                # MCP server configuration + GPIO protection
```

## 📚 Documentation

### Core Documentation
- **[CLAUDE.md](CLAUDE.md)** - Comprehensive development guide for Claude instances
- **[wamr-esp32-string-handling.md](wamr-esp32-string-handling.md)** - String handling issues and solutions
- **[wamr-troubleshooting-guide.md](wamr-troubleshooting-guide.md)** - Major issues resolved and solutions
- **[docs/mcp_architecture.md](docs/mcp_architecture.md)** - MCP protocol implementation details

### Key Topics Covered
- **Memory Management**: PSRAM configuration, WASM stack/heap sizing
- **String Handling**: Data section loading, host function signatures  
- **Stack Issues**: Auxiliary stack overflow resolution
- **Host Functions**: Function naming, parameter conversion
- **Build Configuration**: WASI-SDK, memory layout exports
- **Debugging**: Systematic troubleshooting approaches

## ⚠️ Critical Requirements

### Hardware
- **ESP32-WROVER-E required** - ESP32 without PSRAM has insufficient memory

### Memory Configuration
```c
// PSRAM must be enabled in sdkconfig.defaults
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_MALLOC=y
// ... (see sdkconfig.defaults for complete config)
```

### WASM Stack Size
```bash
# Critical: 32KB stack required for MCP operations
-z stack-size=32768    # In build script
stack_size = 32 * 1024 # In runtime
```

## 🔧 Available MCP Tools

1. **control_led** - Set LED state (pin, state)
2. **get_led_status** - Read current LED state (pin)  
3. **toggle_led** - Toggle LED state (pin)
4. **get_system_info** - ESP32 system information

## 🛡️ GPIO Protection

GPIO protection prevents access to critical system pins via `mcp_config.json`:
- **Protected pins**: 0,1,2,3,6,7,8,9,10,11 (system critical)
- **Input-only pins**: 34,35,36,37,38,39 (ADC pins)
- **Current-limited pins**: 12,13,14,15 (high current risk)

## 🚨 Known Issues and Solutions

### Major Issues Resolved
1. ✅ **String loading corruption** - Fixed with memory layout exports
2. ✅ **Host function name collisions** - Fixed with function renaming  
3. ✅ **Auxiliary stack overflow** - Fixed with 32KB stack size

See **[wamr-troubleshooting-guide.md](wamr-troubleshooting-guide.md)** for complete details.

### Current Status
- ✅ PSRAM enabled (4MB available)
- ✅ WASM compilation and loading
- ✅ Host function calls working
- ✅ MCP protocol implementation  
- ✅ String handling resolved
- ✅ Stack overflow resolved
- 🔧 Buffer management optimization in progress

## 🧪 Testing

### Unit Tests
```bash
./build_mcp_wasm.sh    # Build WASM
idf.py build           # Build firmware
```

### Integration Tests
```bash
./test_mcp_now.sh      # Test MCP endpoints
```

## 🤝 Development Notes

**For future development**, see **[CLAUDE.md](CLAUDE.md)** which contains:
- Complete development guide
- Critical debugging techniques  
- Memory configuration details
- Host function best practices
- Common build issues and solutions

## 📄 License

This project demonstrates ESP32 WASM integration for educational and research purposes.

---

*Successfully implements MCP-over-WASM architecture for ESP32 hardware control after resolving critical WAMR runtime issues.*