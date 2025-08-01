# WAMR ESP32 String Handling Guide

This document consolidates critical information about string literal handling and host function string conversion in WASM-Micro-Runtime (WAMR) on ESP32.

## String Literal Issues and Solutions

### The Problem We Encountered

During MCP implementation, we experienced:
- **String truncation** at exactly 25 characters for specific string literals
- **Complete string corruption** where strings appeared as empty
- **Compiler-specific corruption** - certain string patterns caused WASM data section corruption
- **Memory corruption** happening at compile time, not runtime

**Root Cause**: The WASM compiler was corrupting specific string literals during compilation, particularly the string `"mcp_handle_request: JSON parsed successfully"`.

### Key Discovery

The corruption was resolved by **changing the string content**:
- ❌ **Broken**: `"mcp_handle_request: JSON parsed successfully"`
- ✅ **Working**: `"mcp_handle_request:JSONparsedsuccessfully"`

This indicates a **WASM compiler issue** with specific character patterns, lengths, or string literal merging optimizations.

## WAMR String Literal Storage

### Memory Layout (from WAMR research)

1. **Data Segments**: String literals stored in `WASMDataSeg` structures
2. **Data Area**: First part of linear memory containing global variables and string literals
3. **Stack**: Can overflow and corrupt the data area if not properly configured
4. **Heap**: Can interfere with data section if boundaries not set correctly

### Critical Memory Layout Exports

**Missing from our build** - these exports help WAMR properly separate memory regions:
```bash
-Wl,--export=__heap_base -Wl,--export=__data_end
```

### Memory Configuration Issues

**Our Configuration**:
```bash
-z stack-size=8192         # 8KB stack (build script)
-z stack-size=2048         # 2KB stack (WASM build)
--initial-memory=131072    # 128KB total (build script)  
--initial-memory=65536     # 64KB total (WASM build)
```

**WAMR Samples Use**:
```bash
-z stack-size=4096         # 4KB stack
--initial-memory=65536     # 64KB total
```

**Theory**: Our larger stack (16KB at runtime) may be overwriting the data section where string literals are stored.

### Potential Build Fixes

1. **Add Memory Layout Exports**:
   ```bash
   -Wl,--export=__heap_base -Wl,--export=__data_end
   ```

2. **Prevent String Merging**:
   ```bash
   -fno-merge-all-constants
   ```

3. **Use WASI LibC** (instead of `-nostdlib`)

4. **Enable Memory Profiling**:
   ```bash
   WAMR_ENABLE_MEMORY_PROFILING=1
   ```

## Host Function String Signatures

### String Signature Reference

According to WAMR documentation:
- `$`: Single string parameter (automatic conversion)
- `$$`: Two string parameters (automatic conversion)
- `*`: Buffer address (manual conversion required)
- `~`: Byte length of preceding buffer (must follow `*`)

### Automatic String Conversion (`$`)

**Signature**: `"($)i"` for single string, `"($$)i"` for two strings

```c
// Registration
{"log_info", native_log_info, "($$)i", NULL},

// Implementation
static int32_t native_log_info(wasm_exec_env_t exec_env, const char* tag, const char* message) {
    // Strings are automatically converted - no validation needed
    ESP_LOGI(tag, "[WASM] %s", message);
    return 0;
}
```

**Pros**: Simple, automatic conversion
**Cons**: Limited error handling, potential memory issues

### Manual Address Conversion (`i`)

**Signature**: `"(i)i"` for raw WASM offset

```c
// Registration  
{"log_info2", native_log_info2, "(i)i", NULL},

// Implementation
static int32_t native_log_info2(wasm_exec_env_t exec_env, uint32_t str_offset) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    
    // Validate the string address first
    if (!wasm_runtime_validate_app_str_addr(module_inst, str_offset)) {
        printf("Invalid string address\n");
        return -1;
    }
    
    // Convert to native address
    char *str = wasm_runtime_addr_app_to_native(module_inst, str_offset);
    if (str) {
        printf("Log2: %s\n", str);
        return 0;
    }
    return -1;
}
```

**Pros**: Full control over validation and error handling
**Cons**: More complex implementation

### Buffer with Length (`*~`)

**Signature**: `"(*~)i"` for buffer + length

```c
// Registration
{"log_info3", native_log_info3, "(*~)i", NULL},

// Implementation  
static int32_t native_log_info3(wasm_exec_env_t exec_env, char *str, uint32_t len) {
    // str is already converted, len is the buffer size
    if (str && len > 0) {
        // Ensure null termination
        char temp_buf[len + 1];
        memcpy(temp_buf, str, len);
        temp_buf[len] = '\0';
        printf("Log3: %s\n", temp_buf);
        return 0;
    }
    return -1;
}
```

**Pros**: Safest - explicit length handling, null termination guaranteed
**Cons**: Most complex, requires length parameter

## Critical Host Function Naming Issue

### Function Name Collision Problem

**Issue**: Standard library function names (like `strcmp`) conflict with WASM built-in functions.

**Symptoms**:
- Host function never gets called
- WASM tries to use non-existent built-in function
- "Out of bounds memory access" errors

**Solution**: Rename conflicting functions
```c
// ❌ Problematic
{"strcmp", native_strcmp, "($$)i", NULL},

// ✅ Fixed  
{"wasm_strcmp", native_strcmp, "($$)i", NULL},
```

**WASM Code**:
```c
// ❌ Problematic
extern int strcmp(const char* s1, const char* s2);

// ✅ Fixed
extern int wasm_strcmp(const char* s1, const char* s2);
```

### Functions That Need Renaming

Standard library functions that conflict:
- `strcmp` → `wasm_strcmp`
- `strlen` → `wasm_strlen` (if needed)
- `strcpy` → `wasm_strcpy` (if needed)
- `strcat` → `wasm_strcat` (if needed)

## String Handling Best Practices

### 1. Avoid Standard Library Includes in WASM

**❌ Problematic**:
```c
#include <string.h>  // Causes conflicts with host functions
```

**✅ Correct**:
```c
// Use explicit extern declarations for host functions
extern int wasm_strcmp(const char* s1, const char* s2);
extern int log_info(const char* tag, const char* message);
```

### 2. Choose Appropriate Signature Based on Use Case

- **Simple logging**: Use `($$)i` automatic conversion
- **Critical operations**: Use `(i)i` manual conversion for full control
- **Buffer operations**: Use `(*~)i` with explicit length

### 3. String Literal Content Guidelines

**Avoid**:
- Very long strings (>40 characters)
- Repeated identical strings (compiler may merge incorrectly)
- Complex punctuation patterns

**Prefer**:
- Shorter, unique strings
- Simple character patterns
- Avoid spaces if experiencing issues (use underscores/camelCase)

### 4. Memory Configuration

**Build Configuration**:
```bash
# Add these exports for proper memory layout
-Wl,--export=__heap_base -Wl,--export=__data_end

# Consider string merging prevention
-fno-merge-all-constants

# Keep stack size reasonable (4KB recommended)
-z stack-size=4096
```

**Runtime Configuration**:
```c
// Match WAMR sample configurations
uint32_t stack_size = 4096;   // 4KB stack (not 16KB)
uint32_t heap_size = 8192;    // 8KB heap
```

## Debugging String Issues

### Memory Profiling

Enable WAMR memory profiling:
```c
#define WAMR_ENABLE_MEMORY_PROFILING 1
```

### Validation Functions

Use WAMR's validation functions:
```c
// Validate string address
wasm_runtime_validate_app_str_addr(module_inst, str_offset)

// Validate buffer address  
wasm_runtime_validate_app_addr(module_inst, buffer_offset, length)
```

### Testing Different Approaches

When experiencing string issues, test with all three approaches:
```c
// Test 1: Automatic conversion
log_info("tag", "message");

// Test 2: Manual conversion  
log_info2((uint32_t)"message");

// Test 3: Buffer + length
log_info3("message", strlen("message"));
```

## References

- **WAMR Repository**: `/home/jechternach/src/esp32/wasm-micro-runtime/`
- **Build Documentation**: `/home/jechternach/src/esp32/wasm-micro-runtime/doc/build_wasm_app.md`
- **Memory Tuning**: `/home/jechternach/src/esp32/wasm-micro-runtime/doc/memory_tune.md`
- **Core Structures**: `/home/jechternach/src/esp32/wasm-micro-runtime/core/iwasm/interpreter/wasm.h`
- **ESP32 Integration**: `/home/jechternach/src/esp32/wasm-micro-runtime/product-mini/platforms/esp-idf/`

## Next Steps

1. **Apply build configuration fixes** (memory layout exports, string merging prevention)
2. **Test with reduced stack size** (4KB instead of 16KB)
3. **Implement memory profiling** for debugging remaining issues
4. **Consider WASI LibC** instead of nostdlib for more robust string handling