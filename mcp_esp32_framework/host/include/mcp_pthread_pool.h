#ifndef MCP_PTHREAD_POOL_H
#define MCP_PTHREAD_POOL_H

#include <esp_http_server.h>
#include <pthread.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// MCP request structure for pthread processing
typedef struct {
    char* request_body;
    size_t request_len;
    char* uri;
    char* headers;
    char response[2048];
    
    // Response headers management
    char* response_headers;
    size_t response_headers_capacity;
    size_t response_headers_length;
    
    esp_err_t result;
    bool completed;
    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_cond;
} mcp_request_t;

// MCP WASM Module Management structure (shared between main.c and pthread handler)
typedef struct {
    char route[64];
    char wasm_file[64];
    wasm_module_t wasm_module;
    wasm_module_inst_t wasm_module_inst;
    wasm_exec_env_t exec_env;
    bool loaded;
} mcp_wasm_module_t;

// MCP pthread pool configuration
typedef struct {
    int pool_size;              // Number of worker pthreads
    bool initialized;           // Pool initialization status
    pthread_t* workers;         // Worker pthread handles
    QueueHandle_t request_queue; // FreeRTOS queue for requests
    bool shutdown;              // Shutdown flag
} mcp_pthread_pool_t;

/**
 * @brief Initialize the MCP pthread pool
 * 
 * @param pool_size Number of worker pthreads to create
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_pthread_pool_init(int pool_size);

/**
 * @brief Shutdown the MCP pthread pool
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_pthread_pool_shutdown(void);

/**
 * @brief HTTP handler that uses pthread pool for MCP requests
 * 
 * This handler receives HTTP requests from ESP-IDF HTTP server threads
 * and forwards them to pthread workers for WASM processing.
 * 
 * @param req HTTP request structure
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_pthread_handler(httpd_req_t *req);

/**
 * @brief Get pool statistics for monitoring
 * 
 * @param active_workers Number of currently active workers
 * @param queue_size Current queue size
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_pthread_pool_stats(int* active_workers, int* queue_size);

/**
 * @brief Load WASM module for MCP route
 * 
 * @param route The MCP route (e.g., "/mcp/led")
 * @param wasm_filename The WASM filename to load
 * @return mcp_wasm_module_t* Pointer to loaded module or NULL on failure
 */
mcp_wasm_module_t* load_mcp_wasm_module(const char* route, const char* wasm_filename);

/**
 * @brief Find WASM filename for a given route
 * 
 * @param route The MCP route to look up
 * @return const char* WASM filename or NULL if not found
 */
const char* find_wasm_for_route(const char* route);

#ifdef __cplusplus
}
#endif

#endif // MCP_PTHREAD_POOL_H