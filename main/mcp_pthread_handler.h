#ifndef MCP_PTHREAD_HANDLER_H
#define MCP_PTHREAD_HANDLER_H

#include <esp_http_server.h>
#include <pthread.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#ifdef __cplusplus
extern "C" {
#endif

// MCP request structure for pthread processing
typedef struct {
    char* request_body;
    size_t request_len;
    char* uri;
    char response[2048];
    esp_err_t result;
    bool completed;
    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_cond;
} mcp_request_t;

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

#ifdef __cplusplus
}
#endif

#endif // MCP_PTHREAD_HANDLER_H