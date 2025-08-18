● Excellent analysis! You've identified the core issue perfectly. Let's discuss a flexible solution for making these buffer sizes configurable and moving
  away from stack-based allocation.

  Current Problem Analysis

  Stack Overflow Issues:
  - WASM stack: 64KB total
  - Current stack usage:
    - params[2048] = 2KB
    - tool_params[2048] = 2KB
    - Total: ~4KB just for these buffers
  - At 4096+ bytes each = 8KB+ which is ~12.5% of total stack
  - Plus other function call overhead → stack overflow

  Proposed Solution: Configurable Dynamic Allocation

  1. Configuration System

  Add configurable limits in mcp_sdk.h:

  // Configuration macros - can be overridden at compile time
  #ifndef MCP_PARAMS_BUFFER_SIZE
  #define MCP_PARAMS_BUFFER_SIZE 2048
  #endif

  #ifndef MCP_TOOL_PARAMS_BUFFER_SIZE  
  #define MCP_TOOL_PARAMS_BUFFER_SIZE 2048
  #endif

  #ifndef MCP_RESPONSE_BUFFER_SIZE
  #define MCP_RESPONSE_BUFFER_SIZE 1024
  #endif

  2. Dynamic Allocation Functions

  Create buffer management functions:

  // Internal buffer allocation helpers
  static char* alloc_mcp_buffer(size_t size, const char* name) {
      char* buffer = malloc(size);
      if (!buffer) {
          log_error("MCP", "Failed to allocate buffer for %s", name);
      }
      return buffer;
  }

  static void free_mcp_buffer(char* buffer) {
      if (buffer) free(buffer);
  }

  3. Updated Function Signatures

  In mcp_handle_request():
  int32_t mcp_handle_request(const char* json_request, char* response, int32_t max_len) {
      char method[64];
      char* params = alloc_mcp_buffer(MCP_PARAMS_BUFFER_SIZE, "params");
      if (!params) return create_jsonrpc_error(-32000, "Memory allocation failed", response, max_len);

      // ... existing logic ...

      free_mcp_buffer(params);
      return result;
  }

  In handle_tools_call():
  static int32_t handle_tools_call(const char* params, char* response, int32_t max_len) {
      char tool_name[64];  // Keep small buffers on stack
      char* tool_params = alloc_mcp_buffer(MCP_TOOL_PARAMS_BUFFER_SIZE, "tool_params");
      if (!tool_params) return create_jsonrpc_error(-32000, "Memory allocation failed", response, max_len);

      // ... existing logic ...

      free_mcp_buffer(tool_params);
      return result;
  }

  4. Developer Flexibility

  Compile-time configuration:
  // In project config or CMakeLists.txt
  #define MCP_PARAMS_BUFFER_SIZE 4096
  #define MCP_TOOL_PARAMS_BUFFER_SIZE 4096

  Runtime configuration function:
  // Optional: Runtime configuration
  int32_t mcp_configure_buffers(size_t params_size, size_t tool_params_size) {
      // Store in global config struct
      // Return validation status
  }

  5. Benefits of This Approach

  ✅ Memory Efficiency: Only allocates when needed, frees immediately
  ✅ Stack Safe: Large buffers moved to heap✅ Configurable: Developers can adjust limits per use case
  ✅ Backward Compatible: Default sizes maintain current behavior
  ✅ Error Handling: Clear error messages for allocation failures
  ✅ Scalable: Easy to support larger payloads without code changes

  6. Use Cases for Different Configurations

  Small embedded (limited RAM):
  #define MCP_PARAMS_BUFFER_SIZE 512
  #define MCP_TOOL_PARAMS_BUFFER_SIZE 512

  Music/Large payloads:
  #define MCP_PARAMS_BUFFER_SIZE 4096
  #define MCP_TOOL_PARAMS_BUFFER_SIZE 4096

  Complex applications:
  #define MCP_PARAMS_BUFFER_SIZE 8192
  #define MCP_TOOL_PARAMS_BUFFER_SIZE 8192

  Questions for Implementation:

  1. Error Handling: Should allocation failure be fatal or recoverable?
  2. Global vs Local Config: Store config globally or pass as parameters?
  3. Runtime vs Compile-time: Focus on compile-time config or add runtime?
  4. Naming: Prefer MCP_MAX_* vs MCP_*_BUFFER_SIZE?

  This approach would make the framework much more flexible while solving the immediate stack overflow issue. What are your thoughts on this design?