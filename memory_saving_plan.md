● Memory Optimization Analysis (Analysis Only)

  1. main/main.c - FINDINGS:

  ✅ Good: Only small stack buffers (char error_buf[128]), mostly pointers and primitives
  ✅ Good: pthread stack set to 32KB (reasonable)
  ⚠️ Note: Global variables are minimal

  2. mcp_pthread_pool.c - CRITICAL ISSUES:

● Search(pattern: "char.*\[.*\]|header_buffer|char.*=.*\[", path: "mcp_esp32_framework/host/src/mcp_pthread_pool.c", output_mode: "content")
  ⎿  Found 58 lines (ctrl+r to expand)

● 🚨 CRITICAL STACK BUFFERS in mcp_pthread_pool.c:
  - Line 431: char header_buffer[1024] - 1KB stack allocation per request
  - Line 446: char header_value[256] - 256 bytes
  - Line 539: char header_name[128] - 128 bytes
  - Line 555: char header_value[256] - 256 bytes (another instance)

  3. mcp_http_server.c - CRITICAL ISSUES:

● Search(pattern: "char.*\[.*\]|buffer\[|sse_buffer", path: "mcp_esp32_framework/host/src/mcp_http_server.c", output_mode: "content")
  ⎿  Found 45 lines (ctrl+r to expand)

● 🚨 CRITICAL STACK BUFFERS in mcp_http_server.c:
  - Line 292: char sse_buffer[1024] - 1KB stack allocation for every SSE event
  - Line 403: char demo_data[64] - 64 bytes

  4. mcp_session_manager.c:

● Search(pattern: "char.*\[.*\]|_Thread_local", path: "mcp_esp32_framework/host/src/mcp_session_manager.c", output_mode: "content")
  ⎿  Found 41 lines (ctrl+r to expand)

● ⚠️ MEDIUM ISSUES in mcp_session_manager.c:
  - Line 31: Thread-local storage (37 bytes per thread)
  - Line 447: char search_pattern[64] - 64 bytes stack
  - Line 482: char header_value[37] - small
  - Line 555: char session_id[37] - small
  - Line 595: char ping_data[64] - 64 bytes stack

  5. mcp_host_functions.c:

● Search(pattern: "char.*\[.*\]|event_data\[|buffer.*\[", path: "mcp_esp32_framework/host/src/mcp_host_functions.c", output_mode: "content",
        head_limit: 10)
  ⎿  Found 10 lines (ctrl+r to expand)

● ⚠️ MEDIUM ISSUES in mcp_host_functions.c:
  - Line 642: char event_data[512] - 512 bytes stack allocation

● Update Todos
  ⎿  ☒ Analyze main.c for memory optimizations        
     ☒ Analyze mcp_pthread_pool.c for stack buffers
     ☒ Analyze mcp_http_server.c for large buffers
     ☒ Analyze mcp_session_manager.c for optimizations
     ☒ Analyze mcp_host_functions.c for memory usage
     ☐ Analyze other framework files
     ☐ Summarize priority optimizations

● 6. Other Framework Files:

● Search(pattern: "char.*\[.*\]|buffer.*\[", path: "mcp_esp32_framework/host/src/mcp_config_loader.c", output_mode: "content", head_limit: 8)
  ⎿  Found 8 lines (ctrl+r to expand)

● Search(pattern: "char.*\[.*\]", path: "mcp_esp32_framework/host/src/mcp_wasm_runtime.c", output_mode: "content", head_limit: 5)
  ⎿  Found 2 lines (ctrl+r to expand)

● Update Todos
  ⎿  ☒ Analyze main.c for memory optimizations        
     ☒ Analyze mcp_pthread_pool.c for stack buffers
     ☒ Analyze mcp_http_server.c for large buffers
     ☒ Analyze mcp_session_manager.c for optimizations
     ☒ Analyze mcp_host_functions.c for memory usage
     ☒ Analyze other framework files
     ☐ Summarize priority optimizations

● 🚨 CRITICAL MEMORY OPTIMIZATION OPPORTUNITIES

  TOP PRIORITY - Large Stack Buffers (Move to Heap):

  1. mcp_pthread_pool.c:
  - Line 431: char header_buffer[1024] → 1KB per HTTP request
  - Line 446: char header_value[256] → 256 bytes per request
  - Line 539: char header_name[128] → 128 bytes per request
  - Line 555: char header_value[256] → 256 bytes per request
  - Total per request: ~1.7KB stack usage

  2. mcp_http_server.c:
  - Line 292: char sse_buffer[1024] → 1KB per SSE event

  3. mcp_host_functions.c:
  - Line 642: char event_data[512] → 512 bytes per tool result

  MEDIUM PRIORITY - Thread Stack Size:

  - mcp_pthread_pool.c Line 56: 32KB stack per worker thread
  - Potential: Reduce to 16KB and monitor for overflow

  LOW PRIORITY - Small Buffers (<128 bytes):

  - Various 64-128 byte buffers in config_loader, session_manager
  - Thread-local storage (37 bytes per thread)

  ESTIMATED SAVINGS:

  High Impact Changes:
  1. header_buffer[1024] → heap: Save 1KB stack per request
  2. sse_buffer[1024] → heap: Save 1KB stack per SSE event
  3. Thread stack 32KB → 16KB: Save 16KB per worker thread
  4. event_data[512] → heap: Save 512 bytes per tool call

  Total Potential Savings: ~2.5KB stack per request + 16KB per thread

  With these optimizations, your I2S melody crashes should be significantly reduced due to decreased stack pressure on internal DRAM.
