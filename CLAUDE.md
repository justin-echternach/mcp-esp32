# ESP32 MCP WASM LED Controller - Claude Development Guide

## Project Overview

This is an ESP32 project that implements a **Model Context Protocol (MCP) server** using **WebAssembly (WASM)** modules for LED control. The project enables LLMs to directly control ESP32 hardware through MCP JSON-RPC 2.0 protocol, with the core logic running in sandboxed WASM modules.

**Key Achievement**: Successfully enabled 4MB PSRAM on ESP32-WROVER-E, expanding from ~180KB to 4MB available memory for WASM operations.

## Architecture

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

## Critical Files and Directories

### Core Implementation
- `/main/main.c` - ESP32 main application with HTTP server, WAMR integration, host functions
- `/main/mcp_pthread_handler.c` - MCP protocol HTTP endpoint handlers  
- `/wasm_sdk/mcp_sdk.h` - WASM SDK header defining MCP interfaces and host functions
- `/wasm_sdk/mcp_sdk.c` - WASM SDK implementation with MCP protocol logic
- `/wasm_sdk/example_led_controller.c` - Example LED controller WASM module

### Build System
- `/build_mcp_wasm.sh` - Build script for compiling C to WASM using WASI-SDK
- `/CMakeLists.txt` - ESP32 build configuration (includes PSRAM component)
- `/sdkconfig.defaults` - ESP32 configuration defaults with PSRAM settings

### Configuration
- `/cfg.jsn` - WASM module configuration for ESP32 runtime
- `/mcp_config.json` - MCP server configuration with GPIO protection settings

### Documentation
- `/wamr-esp32-string-handling.md` - **CRITICAL**: Comprehensive guide to WASM string handling issues and solutions
- `/docs/mcp_architecture.md` - MCP protocol implementation details

## Quick Start

### 1. Hardware Setup
- **Required**: ESP32-WROVER-E (with PSRAM)
- SD card formatted FAT32 
- LEDs connected to GPIO pins (configurable)

### 2. Environment Setup
```bash
# ESP-IDF environment
. $HOME/esp/v5.5/esp-idf/export.sh

# WASI SDK (required for WASM compilation)
# Download from: https://github.com/WebAssembly/wasi-sdk/releases
```

### 3. Build and Flash
```bash
# Build WASM module
./build_mcp_wasm.sh

# Build ESP32 firmware  
idf.py build

# Flash to device
idf.py flash

# Monitor logs
idf.py monitor
```

### 4. Test MCP Endpoints
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

## Memory Configuration (CRITICAL)

### PSRAM Setup (ESP32-WROVER-E)
**Problem**: Default ESP32 has only ~180KB free memory, insufficient for WAMR + MCP operations.
**Solution**: Enable PSRAM to get 4MB available memory.

**In `CMakeLists.txt`**:
```cmake
set(COMPONENTS main freertos esptool_py wamr esp_psram)  # ADD esp_psram
```

**In `sdkconfig.defaults`**:
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MODE_QUAD=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_CACHE_WORKAROUND=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
```

### WASM Memory Configuration
**Stack**: 4KB (recommended by WAMR samples)
**Heap**: 16KB (for MCP JSON operations)
**Total Linear Memory**: 64KB

## String Handling Issues (CRITICAL)

**⚠️ MAJOR ISSUE**: WASM compiler corrupts specific string literals during compilation.

### Symptoms
- Strings truncated at exactly 25 characters
- Specific memory addresses (0x814, 0x9cb) contain only null bytes
- Corruption happens at compile time, not runtime

### Root Cause  
WASM compiler issue with specific string patterns. Example:
- ❌ **Broken**: `"mcp_handle_request: JSON parsed successfully"`
- ✅ **Working**: `"mcp_handle_request:JSONparsedsuccessfully"`

### Solutions
1. **Avoid problematic string patterns** (spaces, long strings)
2. **Use shorter, unique strings** with simple characters
3. **Apply build fixes** (see `wamr-esp32-string-handling.md`)

**Critical Reference**: `/wamr-esp32-string-handling.md` contains complete analysis and solutions.

## Host Function Signatures (CRITICAL)

### Function Name Collision Issue
**Problem**: Standard library functions conflict with WASM built-ins.
**Solution**: Rename all conflicting functions.

```c
// ❌ Problematic - causes "out of bounds memory access"
{"strcmp", native_strcmp, "($$)i", NULL},

// ✅ Fixed  
{"wasm_strcmp", native_strcmp, "($$)i", NULL},
```

### String Signature Types
- `$`: Single string (automatic conversion) - `"($)i"`
- `$$`: Two strings (automatic conversion) - `"($$)i"`  
- `*~`: Buffer + length (safest) - `"(*~)i"`
- `i`: Raw offset (manual validation) - `"(i)i"`

**Recommendation**: Use `$$` for simple operations, `*~` for critical operations.

## MCP Protocol Implementation

### Supported Methods
- `initialize` - MCP handshake initialization
- `initialized` - Confirm initialization complete  
- `tools/list` - List available LED control tools
- `tools/call` - Execute LED control operations
- `ping` - Health check

### Available Tools (in WASM module)
1. **control_led** - Set LED state (pin, state)
2. **get_led_status** - Read current LED state (pin)
3. **toggle_led** - Toggle LED state (pin)
4. **get_system_info** - ESP32 system information

## GPIO Protection System

**Configuration**: `/mcp_config.json`
```json
{
  "gpio_protection": {
    "protected_pins": [0, 1, 2, 3, 6, 7, 8, 9, 10, 11],
    "input_only_pins": [34, 35, 36, 37, 38, 39],
    "max_current_pins": [12, 13, 14, 15]
  }
}
```

**Purpose**: Prevent WASM modules from accessing critical system pins.

## Common Build Issues

### 1. WASM Compilation Fails
```bash
# Check WASI SDK path in build_mcp_wasm.sh
WASI_SDK_PATH="/home/jechternach/src/esp32/wasi-sdk-20.0"
```

### 2. ESP32 Build Fails  
```bash
# Ensure ESP-IDF environment is loaded
. $HOME/esp/v5.5/esp-idf/export.sh

# Check CMakeLists.txt includes esp_psram component
```

### 3. WASM Module Crashes
- Check string handling (see `wamr-esp32-string-handling.md`)
- Verify host function signatures use automatic conversion (`$`)
- Ensure no standard library conflicts (rename functions)

### 4. Memory Issues
- Confirm PSRAM is enabled and working
- Check WASM stack/heap sizes in build script
- Monitor ESP32 heap usage in logs

## Debugging

### WASM Memory Issues
```c
// Add to main.c for memory debugging
wasm_runtime_dump_line_buf_impl(...);  // Memory layout
wasm_runtime_validate_app_str_addr(...); // String validation
```

### String Corruption Debugging  
```c
// Direct string literal access test (in WASM)
const char* test_str = "problematic_string";
log_info("DEBUG", test_str);  // Check if empty/corrupted
```

### Host Function Validation
```c
// Add validation to all host functions
static int32_t native_log_info(wasm_exec_env_t exec_env, const char* tag, const char* message) {
    if (!tag || !message) {
        printf("ERROR: NULL string parameters\n");
        return -1;
    }
    ESP_LOGI(tag, "[WASM] %s", message);
    return 0;
}
```

## Testing

### Unit Tests
```bash
# Test WASM compilation
./build_mcp_wasm.sh

# Test ESP32 build
idf.py build
```

### Integration Tests  
```bash
# Test MCP endpoints
./test_mcp_now.sh
```

### Manual Testing
1. Flash firmware to ESP32
2. Connect to WiFi (check serial logs for IP)
3. Test MCP endpoints with curl
4. Verify LED control works
5. Check SSE events (optional)

## Current Status

### Working
- ✅ PSRAM enabled (4MB available memory)
- ✅ WASM compilation and loading
- ✅ Host function calls (with proper signatures)
- ✅ MCP protocol implementation
- ✅ GPIO protection system
- ✅ HTTP server with MCP endpoints

### Known Issues
- ⚠️ **String corruption** at specific memory addresses (0x814, 0x9cb)
- ⚠️ **WASM compiler** corrupts certain string literals
- ⚠️ **Memory layout** may need optimization for larger WASM modules

### Pending Tasks (from todo list)
- Test complete MCP flow with curl
- Investigate remaining string corruption at specific addresses
- Optimize memory layout for production use

## References

- **WAMR Documentation**: `/home/jechternach/src/esp32/wasm-micro-runtime/`
- **ESP-IDF GPIO Guide**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/gpio.html
- **MCP Specification**: https://spec.modelcontextprotocol.io/specification/
- **WASI SDK**: https://github.com/WebAssembly/wasi-sdk

## Development Notes for Future Claude Instances

1. **Always check string handling first** - Most WASM crashes are string-related
2. **Use automatic string conversion (`$`)** unless you need manual control
3. **Rename standard library functions** to avoid conflicts
4. **Verify PSRAM is working** before debugging memory issues
5. **Reference `wamr-esp32-string-handling.md`** for complete string handling guide
6. **Test with simple strings first** before using complex JSON
7. **Monitor ESP32 heap usage** - WAMR requires significant memory
8. **Build incrementally** - Test WASM compilation before ESP32 integration

## Security Considerations

- GPIO protection prevents access to critical system pins
- WASM sandboxing isolates LED control logic
- No network access from WASM modules (only GPIO/system functions)
- HTTP endpoints should be secured in production (add authentication)

---

*This documentation reflects the current state after extensive debugging of WASM string handling issues and successful PSRAM integration. The project demonstrates a working MCP-over-WASM architecture for ESP32 hardware control.*