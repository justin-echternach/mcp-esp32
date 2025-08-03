# ESP32 MCP WASM LED Controller

## 🔥 [CURRENT PLAN - PAY ATTENTION](docs/kickass-demo-plan.md) 🔥

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


Current Refactor Tasks ...

 I definitely want to use the PAM8403.  The first thing we need to do is clean up the current project, so I can 
  basically use it as a framework for other projects.  The wasm part needs a little cleanup but main/main.c and the 
  associated file will need to be refactored so that when i create a new ESP-IDF project I can just include what is 
  needed for the http server, mcp, host functions etc.  Don't change anything yet.  Just analyze what needs to be done.

● Excellent idea to refactor into a reusable framework! Let me analyze what needs to be done to clean up the codebase:

  Current Structure Analysis

  main/main.c (3000+ lines - way too monolithic!)

  Currently contains:
  1. WiFi setup (lines ~1150-1230)
  2. HTTP server (lines ~480-1140)
  3. WAMR initialization (lines ~2075-2330)
  4. Host functions (lines ~1234-1820)
  5. MCP config loading (lines ~160-440)
  6. GPIO/Hardware specific (lines ~2095-2110)
  7. WASM module management (lines ~590-900)
  8. SD card mounting (lines ~1900-2050)
  9. App main + pthread setup (lines ~2340-2400)

  What Needs Refactoring

  1. Core MCP Framework (reusable)

  mcp_framework/
  ├── include/
  │   ├── mcp_server.h          # MCP server interface
  │   ├── mcp_wasm_runtime.h    # WAMR wrapper
  │   ├── mcp_host_functions.h  # Host function registry
  │   └── mcp_config.h          # Config structures
  ├── src/
  │   ├── mcp_server.c          # HTTP server + MCP endpoints
  │   ├── mcp_wasm_runtime.c    # WAMR init/management
  │   ├── mcp_host_registry.c   # Core host functions
  │   └── mcp_config_loader.c   # JSON config parsing
  └── CMakeLists.txt

  2. Project-Specific Implementation

  main/
  ├── main.c                     # Just app_main + project setup
  ├── project_host_functions.c   # LED, GPIO, speaker functions
  ├── hardware_init.c            # GPIO setup, peripherals
  └── CMakeLists.txt

  Specific Refactoring Tasks

  A. Extract Core Components

  1. MCP Server Module
    - HTTP server setup
    - MCP route handling
    - CORS support
    - Request/response processing
  2. WASM Runtime Module
    - WAMR initialization
    - Module loading/unloading
    - Execution environment management
    - Memory configuration
  3. Host Function Registry
    - Core functions (malloc, string ops, logging)
    - Registration mechanism
    - Type signature handling
  4. Configuration System
    - JSON parsing
    - Route mapping
    - GPIO protection rules

  B. Define Clean Interfaces

  // mcp_server.h
  typedef struct {
      char* wifi_ssid;
      char* wifi_pass;
      int port;
      mcp_route_t* routes;
      int route_count;
  } mcp_server_config_t;

  esp_err_t mcp_server_init(mcp_server_config_t* config);
  esp_err_t mcp_server_start(void);
  esp_err_t mcp_server_register_host_functions(host_function_t* funcs, int count);

  C. Separate Concerns

  Remove from Core:
  - LED blink loop (project-specific)
  - GPIO pin definitions
  - Hardware-specific initialization
  - Test/debug code

  Keep in Core:
  - HTTP server infrastructure
  - WASM runtime management
  - MCP protocol handling
  - Core host functions (memory, strings, logging)

  D. Make Hardware Agnostic

  Current code has hardcoded:
  - LED_GPIO GPIO_NUM_5
  - SD card pins
  - LED state management

  Should be:
  - Configuration-driven
  - Optional modules
  - Hardware abstraction layer

  Cleanup Tasks

  1. Remove Dead Code
    - SSE implementation (currently disabled)
    - Legacy LED control functions
    - Duplicate error handling
  2. Consolidate Headers
    - Too many includes
    - Create proper header organization
    - Forward declarations
  3. Fix Threading Model
    - Clear separation of pthread vs main thread
    - Document thread safety requirements
    - Proper mutex usage
  4. Improve Error Handling
    - Consistent error codes
    - Better error propagation
    - Cleanup on failure paths
  5. Memory Management
    - Clear ownership rules
    - Consistent allocation strategy
    - PSRAM vs regular RAM usage

  Benefits After Refactoring

  1. New Project Setup:
  // main.c for new project
  #include "mcp_framework.h"
  #include "my_hardware.h"

  void app_main() {
      // Initialize hardware
      my_hardware_init();

      // Setup MCP server
      mcp_server_config_t config = {
          .wifi_ssid = "MySSID",
          .wifi_pass = "MyPass",
          .port = 80
      };

      // Register project-specific host functions
      mcp_server_register_host_functions(my_host_functions, count);

      // Start server
      mcp_server_init(&config);
      mcp_server_start();
  }

  2. Modular WASM Modules
    - Drop in new .was files
    - Auto-discovery from SD card
    - No recompilation needed
  3. Easy Hardware Changes
    - Just implement new host functions
    - No core framework changes
    - Clean abstraction

  This refactoring will make your RGB LED + Music demo much cleaner to implement!