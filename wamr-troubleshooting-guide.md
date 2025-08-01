# WAMR ESP32 Troubleshooting Guide - Major Issues and Solutions

This document records the critical issues encountered and solutions implemented during ESP32 MCP WASM development, serving as a comprehensive troubleshooting reference.

## 🎯 Summary of Major Achievements

**Successfully resolved three critical WAMR runtime issues:**
1. ✅ **String Literal Loading Failure** - Strings corrupted during data section loading
2. ✅ **Host Function Name Collisions** - Standard library conflicts causing crashes  
3. ✅ **Auxiliary Stack Overflow** - Insufficient stack for complex function calls

## 🚨 Issue #1: String Literal Data Section Loading Failure

### Problem Description
String literals compiled correctly into WASM file but appeared as empty/null at runtime. Specific memory addresses (0x814, 0x9cb) contained only zeros instead of expected string data.

### Symptoms
```c
// Compiled correctly in WASM file at offset 0x16d1
const char* debug_msg = "mcp_handle_request:About to check tools/list";

// But at runtime showed as empty
log_info("DEBUG", debug_msg);  // Logs: ""
```

### Root Cause Analysis
**WAMR data section boundary confusion** - WAMR didn't know proper memory layout boundaries, causing:
- Incomplete data section loading from WASM file
- Stack/heap initialization overwriting data section
- Strings below address ~0x1000 failing to load properly

### Solution: Memory Layout Exports
**In `build_mcp_wasm.sh`:**
```bash
-Wl,--export=__data_end \        # Export end of data section
-Wl,--export=__heap_base \       # Export heap start boundary  
-Wl,--global-base=4096 \         # Force globals above 4KB boundary
```

### Results
- **Before**: Strings at 0x814, 0x9cb = empty
- **After**: Strings at 0x1414, 0x1758 = working correctly
- **Boundary shift**: All data moved above 4KB, into working memory region

### Verification Method
```c
// File buffer inspection before/after wasm_runtime_load
// Confirmed: String exists in WASM file but wasn't loading to runtime memory
// After fix: String loads correctly at higher addresses
```

## 🚨 Issue #2: Host Function Name Collisions 

### Problem Description
Standard library function names (strcmp, strlen, etc.) conflicted with WASM built-in functions, causing "out of bounds memory access" errors.

### Symptoms
```c
// This would crash with "out of bounds memory access"
if (strcmp(method, "tools/list") == 0) { ... }

// Host function never called - WASM tried to use non-existent built-in
```

### Root Cause Analysis
**Function name collision** - WASM compiler prioritized built-in functions over host-provided functions, but built-ins didn't exist, causing crash.

### Solution: Function Renaming
**In host registration (main.c):**
```c
// ❌ Problematic
{"strcmp", native_strcmp, "($$)i", NULL},

// ✅ Fixed  
{"wasm_strcmp", native_strcmp, "($$)i", NULL},
```

**In WASM code:**
```c
// ❌ Problematic
extern int strcmp(const char* s1, const char* s2);

// ✅ Fixed
extern int wasm_strcmp(const char* s1, const char* s2);
```

### Additional Fixes
**Removed standard library includes:**
```c
// ❌ Caused conflicts
#include <string.h>

// ✅ Use explicit extern declarations
extern int wasm_strcmp(const char* s1, const char* s2);
extern int log_info(const char* tag, const char* message);
```

## 🚨 Issue #3: WASM Auxiliary Stack Overflow

### Problem Description
Complex MCP JSON processing caused "wasm auxiliary stack overflow" before any user code executed.

### Symptoms
```
Exception: wasm auxiliary stack overflow
```
- Function call setup failed before reaching user code
- Direct string tests worked, but function calls with parameters failed

### Root Cause Analysis
**Insufficient auxiliary stack space** for complex function call chains:
- **Auxiliary stack** = fraction of main stack (typically 1/4 to 1/8)
- **Our config**: 8KB main → ~2KB auxiliary (too small)  
- **MCP processing**: Deep function calls + host function overhead + JSON parsing

### Solution: Increase Stack Size to 32KB
**Reference**: [WAMR GitHub Issue #3071](https://github.com/bytecodealliance/wasm-micro-runtime/issues/3071)

**In `build_mcp_wasm.sh`:**
```bash
-z stack-size=32768 \    # 32KB compile-time stack
```

**In `main.c`:**
```c
uint32_t stack_size = 32 * 1024;  // 32KB runtime stack
```

### Results
- **Before**: 8KB main → ~2KB auxiliary → Stack overflow
- **After**: 32KB main → ~8KB auxiliary → Success
- **Function calls**: Now handle complex MCP JSON processing chains

### Stack Size Guidelines
Based on WAMR community experience:
- **Simple operations**: 8KB main stack
- **Moderate complexity**: 16-32KB main stack  
- **Complex JSON/deep calls**: 32-64KB main stack
- **Very complex processing**: 64KB+ main stack

## 🔧 Host Function Signature Best Practices

### Automatic String Conversion (Recommended)
```c
// Registration
{"log_info", native_log_info, "($$)i", NULL},

// Implementation  
static int32_t native_log_info(wasm_exec_env_t exec_env, const char* tag, const char* message) {
    ESP_LOGI(tag, "[WASM] %s", message);
    return 0;
}
```

### Manual Address Validation (For Critical Operations)
```c
// Registration
{"log_info_safe", native_log_info_safe, "(i)i", NULL},

// Implementation
static int32_t native_log_info_safe(wasm_exec_env_t exec_env, uint32_t str_offset) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    if (!wasm_runtime_validate_app_addr(module_inst, str_offset, 1)) {
        printf("Invalid string address: 0x%x\n", str_offset);
        return -1;
    }
    
    char *str = wasm_runtime_addr_app_to_native(module_inst, str_offset);
    printf("Safe log: %s\n", str);
    return 0;
}
```

## 📋 Diagnostic and Debugging Techniques

### 1. String Loading Verification
```c
// Check if string exists in WASM file buffer
for (size_t i = 0; i <= file_size - target_len; i++) {
    if (memcmp(wasm_file_buf + i, target_string, target_len) == 0) {
        ESP_LOGI(TAG, "String found at file offset 0x%zx", i);
    }
}
```

### 2. Runtime String Address Inspection  
```c
// Inspect WASM memory at problematic addresses
LOG_INFO: tag_valid=1, msg_valid=1
LOG_INFO: tag='DEBUG', message=''  // Empty = corruption
LOG_INFO: Native ptr for msg_wasm_addr 0x814 = 0x3ffecedc
LOG_INFO: Raw bytes: 00 00 00 00 00 00 00 00  // All zeros = not loaded
```

### 3. Stack Overflow Detection
```
Exception: wasm auxiliary stack overflow
```
- Indicates: Function call setup failure, not user code issue
- Solution: Increase main stack size significantly (32KB+)

### 4. Memory Layout Analysis
```c
=== WAMR MEMORY LAYOUT DEBUG ===
MEMORY: total_size=65536 bytes (0x10000)
PROBLEM ADDRESSES:
  0x814 = 2068 decimal (in first 2068 bytes) 
  0x9cb = 2507 decimal (in first 2507 bytes)
ESTIMATED MEMORY REGIONS:
  Data section: 0x0 - ~0x2000 (0-8192)
  >> Problematic addresses in data section - should contain strings!
```

## 🎯 Build Configuration Template

### Working WASM Build Configuration
```bash
"$WASI_SDK_PATH/bin/clang" \
    --target=wasm32-wasi \
    -O3 \
    -z stack-size=32768 \              # 32KB stack for complex operations
    -Wl,--initial-memory=65536 \       # 64KB linear memory
    -nostdlib \                        # Avoid standard library conflicts
    -Wl,--no-entry \
    -Wl,--export=handle_mcp_request \
    -Wl,--export=_start \
    -Wl,--export=__data_end \          # Critical: Data section boundary
    -Wl,--export=__heap_base \         # Critical: Heap start boundary
    -Wl,--global-base=4096 \           # Critical: Force data above 4KB
    -Wl,--strip-all \
    -Wl,--allow-undefined \
    -o wasm/output.wasm \
    source1.c source2.c
```

### ESP32 WAMR Runtime Configuration
```c
// Memory allocation
uint32_t stack_size = 32 * 1024;  // 32KB - matches build config
uint32_t heap_size = 0;            // Use linear memory for heap

// Instantiation
wasm_module_inst_t module_inst = wasm_runtime_instantiate(
    wasm_module, stack_size, heap_size, error_buf, sizeof(error_buf)
);
```

## 🚀 Success Indicators

### 1. String Loading Success
```
✅ String still present at same location after wasm_runtime_load
LOG_INFO: tag='DIRECT_TEST1', message='mcp_handle_request:About to check tools/list'
```

### 2. Stack Overflow Resolution  
```
✅ No "wasm auxiliary stack overflow" errors
✅ Function calls execute successfully
LOG_INFO: tag='MCP', message='mcp_handle_request: Starting'
```

### 3. Host Function Success
```
✅ Host functions called without "out of bounds memory access"
[WASM]: strcmp called!  # wasm_strcmp working correctly
```

## 📚 References and Resources

- **WAMR Repository**: `/home/jechternach/src/esp32/wasm-micro-runtime/`
- **Auxiliary Stack Issue**: [WAMR GitHub Issue #3071](https://github.com/bytecodealliance/wasm-micro-runtime/issues/3071)
- **WAMR Build Guide**: `/home/jechternach/src/esp32/wasm-micro-runtime/doc/build_wasm_app.md`
- **ESP32 Integration**: `/home/jechternach/src/esp32/wasm-micro-runtime/product-mini/platforms/esp-idf/`
- **String Handling Guide**: `wamr-esp32-string-handling.md`

## 🔮 Lessons Learned

1. **Always check memory layout exports first** for string loading issues
2. **Rename standard library functions** to avoid WASM built-in conflicts  
3. **Use 32KB+ stack for complex operations** - auxiliary stack needs significant headroom
4. **Test with direct string access** to isolate data loading vs processing issues
5. **Monitor WAMR GitHub issues** for known problems and solutions
6. **Use systematic debugging** - isolate file vs runtime vs processing issues

---

*This guide represents extensive debugging work resolving fundamental WAMR integration issues on ESP32. All solutions have been tested and verified working.*