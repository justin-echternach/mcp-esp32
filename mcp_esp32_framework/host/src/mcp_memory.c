#include "mcp_memory.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "MCP_MEMORY";

// Memory alignment for WAMR compatibility
#define WASM_BUFFER_ALIGNMENT 4

// Memory thresholds for smart allocation decisions
#define LARGE_BUFFER_THRESHOLD 8192 // 8KB - prefer PSRAM for buffers larger than this
#define PSRAM_RESERVE_BYTES 32768   // Reserve 32KB PSRAM headroom

// ========== Core Memory Allocation Functions ==========

void *mcp_malloc(size_t size)
{
    if (size == 0)
    {
        ESP_LOGW(TAG, "mcp_malloc called with size 0");
        return NULL;
    }

    void *ptr = NULL;

#ifdef CONFIG_SPIRAM
    // For general allocation, prefer PSRAM for larger allocations
    if (size >= LARGE_BUFFER_THRESHOLD)
    {
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (psram_free > size + PSRAM_RESERVE_BYTES)
        {
            ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
            if (ptr)
            {
                ESP_LOGV(TAG, "Allocated %zu bytes in PSRAM at %p", size, ptr);
                return ptr;
            }
        }
    }
#endif

    // Fallback to DRAM or for small allocations
    ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (ptr)
    {
        ESP_LOGV(TAG, "Allocated %zu bytes in DRAM at %p", size, ptr);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes", size);
    }

    return ptr;
}

void *mcp_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr = mcp_malloc(total);
    if (ptr)
    {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *mcp_malloc_aligned(size_t alignment, size_t size)
{
    if (size == 0)
    {
        ESP_LOGW(TAG, "mcp_malloc_aligned called with size 0");
        return NULL;
    }

    void *ptr = NULL;

#ifdef CONFIG_SPIRAM
    // For aligned allocation, prefer PSRAM for larger allocations
    if (size >= LARGE_BUFFER_THRESHOLD)
    {
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (psram_free > size + PSRAM_RESERVE_BYTES)
        {
            ptr = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM);
            if (ptr)
            {
                ESP_LOGV(TAG, "Allocated %zu bytes (aligned %zu) in PSRAM at %p", size, alignment, ptr);
                return ptr;
            }
        }
    }
#endif

    // Fallback to DRAM
    ptr = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_8BIT);
    if (ptr)
    {
        ESP_LOGV(TAG, "Allocated %zu bytes (aligned %zu) in DRAM at %p", size, alignment, ptr);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes (aligned %zu)", size, alignment);
    }

    return ptr;
}

void *mcp_malloc_wasm_buffer(size_t size)
{
    // Smart allocation strategy for WASM file buffers
    // - Prefer PSRAM for large buffers
    // - 4-byte alignment for WAMR compatibility
    // - Fallback to DRAM if needed
    // - Logging and error handling

    if (size == 0)
    {
        ESP_LOGW(TAG, "mcp_malloc_wasm_buffer called with size 0");
        return NULL;
    }

    void *ptr = NULL;

#ifdef CONFIG_SPIRAM
    // Always prefer PSRAM for WASM buffers - provides isolation and abundant space
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free > size + PSRAM_RESERVE_BYTES)
    {
        ptr = heap_caps_aligned_alloc(WASM_BUFFER_ALIGNMENT, size, MALLOC_CAP_SPIRAM);
        if (ptr)
        {
            ESP_LOGI(TAG, "Allocated %zu bytes WASM buffer in PSRAM at %p (PSRAM free: %zu KB)",
                     size, ptr, psram_free / 1024);
            return ptr;
        }
        else
        {
            ESP_LOGW(TAG, "PSRAM allocation failed for WASM buffer, trying DRAM");
        }
    }
    else
    {
        ESP_LOGW(TAG, "PSRAM low (%zu KB), using DRAM for WASM buffer", psram_free / 1024);
    }
#endif

    // Fallback to DRAM with 4-byte alignment
    ptr = heap_caps_aligned_alloc(WASM_BUFFER_ALIGNMENT, size, MALLOC_CAP_8BIT);
    if (ptr)
    {
        ESP_LOGI(TAG, "Allocated %zu bytes WASM buffer in DRAM at %p", size, ptr);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes WASM buffer in both PSRAM and DRAM", size);
    }

    return ptr;
}

void *mcp_malloc_response_buffer(size_t size)
{
    // Always use PSRAM for response buffers (isolation)
    // - Prevents corruption issues you discovered

    if (size == 0)
    {
        ESP_LOGW(TAG, "mcp_malloc_response_buffer called with size 0");
        return NULL;
    }

    void *ptr = NULL;

#ifdef CONFIG_SPIRAM
    // Force PSRAM for response buffers - critical for memory isolation
    ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (ptr)
    {
        ESP_LOGV(TAG, "Allocated %zu bytes response buffer in PSRAM at %p", size, ptr);
        return ptr;
    }
    else
    {
        ESP_LOGW(TAG, "PSRAM allocation failed for response buffer, trying DRAM (less safe)");
    }
#endif

    // Emergency fallback to DRAM (less safe due to potential corruption)
    ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (ptr)
    {
        ESP_LOGW(TAG, "Allocated %zu bytes response buffer in DRAM at %p (corruption risk)", size, ptr);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes response buffer", size);
    }

    return ptr;
}

void *mcp_malloc_psram(size_t size)
{
    if (size == 0)
    {
        ESP_LOGW(TAG, "mcp_malloc_psram called with size 0");
        return NULL;
    }

#ifdef CONFIG_SPIRAM
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (ptr)
    {
        ESP_LOGV(TAG, "Allocated %zu bytes in PSRAM at %p", size, ptr);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes in PSRAM", size);
    }
    return ptr;
#else
    ESP_LOGE(TAG, "PSRAM not available, cannot allocate %zu bytes", size);
    return NULL;
#endif
}

void *mcp_malloc_dram(size_t size)
{
    if (size == 0)
    {
        ESP_LOGW(TAG, "mcp_malloc_dram called with size 0");
        return NULL;
    }

    void *ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (ptr)
    {
        ESP_LOGV(TAG, "Allocated %zu bytes in DRAM at %p", size, ptr);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes in DRAM", size);
    }

    return ptr;
}

void *mcp_malloc_module_registry(size_t count, size_t element_size)
{
    size_t total_size = count * element_size;

    if (total_size == 0)
    {
        ESP_LOGW(TAG, "mcp_malloc_module_registry called with zero total size");
        return NULL;
    }

    void *ptr = NULL;

#ifdef CONFIG_SPIRAM
    // Always use PSRAM for module registry - provides isolation
    ptr = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
    if (ptr)
    {
        ESP_LOGI(TAG, "Allocated module registry for %zu elements (%zu bytes) in PSRAM at %p",
                 count, total_size, ptr);
        memset(ptr, 0, total_size); // Initialize to zero
        return ptr;
    }
    else
    {
        ESP_LOGW(TAG, "PSRAM allocation failed for module registry, trying DRAM");
    }
#endif

    // Fallback to DRAM
    ptr = heap_caps_malloc(total_size, MALLOC_CAP_8BIT);
    if (ptr)
    {
        ESP_LOGI(TAG, "Allocated module registry for %zu elements (%zu bytes) in DRAM at %p",
                 count, total_size, ptr);
        memset(ptr, 0, total_size); // Initialize to zero
    }
    else
    {
        ESP_LOGE(TAG, "Failed to allocate module registry for %zu elements (%zu bytes)",
                 count, total_size);
    }

    return ptr;
}

void mcp_free(void *ptr)
{
    if (!ptr)
    {
        ESP_LOGV(TAG, "mcp_free called with NULL pointer");
        return;
    }

    ESP_LOGV(TAG, "Freeing memory at %p", ptr);
    heap_caps_free(ptr);
}

// ========== Memory Information Functions ==========

size_t mcp_get_free_memory(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

size_t mcp_get_free_psram(void)
{
#ifdef CONFIG_SPIRAM
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

size_t mcp_get_largest_free_block(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

size_t mcp_get_largest_free_psram_block(void)
{
#ifdef CONFIG_SPIRAM
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

void mcp_memory_stats(void)
{
    size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t dram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "=== MCP Memory Statistics ===");
    ESP_LOGI(TAG, "DRAM: Free=%zu KB, Largest Block=%zu KB", dram_free / 1024, dram_largest / 1024);

#ifdef CONFIG_SPIRAM
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM: Free=%zu KB, Largest Block=%zu KB", psram_free / 1024, psram_largest / 1024);
    ESP_LOGI(TAG, "Total Free: %zu KB", (dram_free + psram_free) / 1024);
#else
    ESP_LOGI(TAG, "PSRAM: Not available");
    ESP_LOGI(TAG, "Total Free: %zu KB", dram_free / 1024);
#endif

    ESP_LOGI(TAG, "===============================");
}

bool mcp_memory_init(void)
{
    ESP_LOGI(TAG, "Initializing MCP memory management");

    // Log initial memory state
    mcp_memory_stats();

#ifdef CONFIG_SPIRAM
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free < 1024 * 1024)
    { // Less than 1MB PSRAM available
        ESP_LOGW(TAG, "Low PSRAM availability: %zu KB", psram_free / 1024);
    }
#endif

    size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (dram_free < 64 * 1024)
    { // Less than 64KB DRAM available
        ESP_LOGW(TAG, "Low DRAM availability: %zu KB", dram_free / 1024);
    }

    ESP_LOGI(TAG, "MCP memory management initialized successfully");
    return true;
}