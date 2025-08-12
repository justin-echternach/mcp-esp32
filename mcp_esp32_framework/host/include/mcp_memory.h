#ifndef MCP_MEMORY_H
#define MCP_MEMORY_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ========== Core Memory Allocation Functions ==========

/**
 * @brief Smart general-purpose memory allocation
 * 
 * Automatically chooses between DRAM and PSRAM based on size and availability:
 * - Small allocations (<8KB): Prefers DRAM for speed
 * - Large allocations (>=8KB): Prefers PSRAM for capacity
 * - Always falls back gracefully
 * 
 * @param size Size to allocate in bytes
 * @return Pointer to allocated memory or NULL on failure
 */
void* mcp_malloc(size_t size);

/**
 * @brief Smart zero-initialized memory allocation
 * 
 * Allocates memory for an array of elements and zeros all bytes.
 * Uses the same smart allocation strategy as mcp_malloc():
 * - Small allocations (<8KB): Prefers DRAM for speed
 * - Large allocations (>=8KB): Prefers PSRAM for capacity
 * - Always falls back gracefully
 * - Automatically zeros the allocated memory
 * 
 * @param count Number of elements to allocate
 * @param size Size of each element in bytes
 * @return Pointer to zero-initialized memory or NULL on failure
 */
void* mcp_calloc(size_t count, size_t size);

/**
 * @brief Smart aligned memory allocation
 * 
 * Same smart allocation strategy as mcp_malloc but with alignment guarantee.
 * Useful for DMA buffers or performance-critical data structures.
 * 
 * @param alignment Alignment requirement (must be power of 2)
 * @param size Size to allocate in bytes
 * @return Pointer to aligned allocated memory or NULL on failure
 */
void* mcp_malloc_aligned(size_t alignment, size_t size);

/**
 * @brief Specialized WASM file buffer allocation
 * 
 * Optimized for WASM module file buffers with specific requirements:
 * - 4-byte alignment for WAMR AOT compatibility
 * - Prefers PSRAM for memory isolation and capacity
 * - Falls back to DRAM if PSRAM unavailable
 * - Must be kept alive until wasm_runtime_unload()
 * 
 * @param size Size of WASM file buffer in bytes
 * @return Pointer to aligned WASM buffer or NULL on failure
 */
void* mcp_malloc_wasm_buffer(size_t size);

/**
 * @brief Specialized response buffer allocation
 * 
 * Designed for HTTP response buffers with memory isolation requirements:
 * - Forces PSRAM allocation to prevent WAMR corruption
 * - Based on debugging experience with export table corruption
 * - Emergency fallback to DRAM with warning
 * 
 * @param size Size of response buffer in bytes
 * @return Pointer to response buffer or NULL on failure
 */
void* mcp_malloc_response_buffer(size_t size);

/**
 * @brief Force PSRAM allocation
 * 
 * Always allocates from PSRAM, no fallback to DRAM.
 * Fails if PSRAM unavailable or full.
 * 
 * @param size Size to allocate in bytes
 * @return Pointer to PSRAM memory or NULL on failure
 */
void* mcp_malloc_psram(size_t size);

/**
 * @brief Force DRAM allocation
 * 
 * Always allocates from internal DRAM, no PSRAM usage.
 * Use for time-critical or DMA-required allocations.
 * 
 * @param size Size to allocate in bytes
 * @return Pointer to DRAM memory or NULL on failure
 */
void* mcp_malloc_dram(size_t size);

/**
 * @brief Specialized module registry allocation
 * 
 * Allocates and initializes memory for WASM module registry:
 * - Prefers PSRAM for isolation from system memory
 * - Automatically zeros the allocated memory
 * - Calculates total size from count and element size
 * 
 * @param count Number of elements in the registry
 * @param element_size Size of each registry element
 * @return Pointer to initialized registry memory or NULL on failure
 */
void* mcp_malloc_module_registry(size_t count, size_t element_size);

/**
 * @brief Free memory allocated by any mcp_malloc_* function
 * 
 * Safe to call with NULL pointer (no-op).
 * Automatically detects and frees from correct heap (DRAM/PSRAM).
 * 
 * @param ptr Pointer to memory to free (or NULL)
 */
void mcp_free(void* ptr);

// ========== Memory Information Functions ==========

/**
 * @brief Get free DRAM memory
 * @return Free DRAM bytes
 */
size_t mcp_get_free_memory(void);

/**
 * @brief Get free PSRAM memory
 * @return Free PSRAM bytes (0 if PSRAM not available)
 */
size_t mcp_get_free_psram(void);

/**
 * @brief Get largest free DRAM block
 * @return Largest contiguous DRAM block in bytes
 */
size_t mcp_get_largest_free_block(void);

/**
 * @brief Get largest free PSRAM block
 * @return Largest contiguous PSRAM block in bytes (0 if PSRAM not available)
 */
size_t mcp_get_largest_free_psram_block(void);

/**
 * @brief Print comprehensive memory statistics
 * 
 * Logs detailed memory usage information including:
 * - DRAM free space and largest block
 * - PSRAM free space and largest block (if available)
 * - Total available memory
 */
void mcp_memory_stats(void);

/**
 * @brief Initialize MCP memory management system
 * 
 * Call once at startup to initialize memory management and log initial state.
 * Checks for low memory conditions and logs warnings.
 * 
 * @return true on success, false on failure
 */
bool mcp_memory_init(void);

// ========== Convenience Macros ==========

/**
 * @brief Allocate and cast to specific type
 * 
 * Example: my_struct_t* ptr = MCP_MALLOC_TYPE(my_struct_t);
 */
#define MCP_MALLOC_TYPE(type) ((type*)mcp_malloc(sizeof(type)))

/**
 * @brief Allocate array and cast to specific type
 * 
 * Example: int* array = MCP_MALLOC_ARRAY(int, 100);
 */
#define MCP_MALLOC_ARRAY(type, count) ((type*)mcp_malloc(sizeof(type) * (count)))

/**
 * @brief Allocate WASM buffer and cast to uint8_t*
 * 
 * Example: uint8_t* buffer = MCP_MALLOC_WASM_BUFFER(file_size);
 */
#define MCP_MALLOC_WASM_BUFFER(size) ((uint8_t*)mcp_malloc_wasm_buffer(size))

/**
 * @brief Allocate response buffer and cast to char*
 * 
 * Example: char* response = MCP_MALLOC_RESPONSE_BUFFER(2048);
 */
#define MCP_MALLOC_RESPONSE_BUFFER(size) ((char*)mcp_malloc_response_buffer(size))

#ifdef __cplusplus
}
#endif

#endif // MCP_MEMORY_H