#ifndef MCP_HTTP_SERVER_H
#define MCP_HTTP_SERVER_H

#include "esp_http_server.h"
#include "esp_err.h"
#include "mcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP server configuration for MCP framework
 */
typedef struct {
    uint16_t port;                  ///< HTTP server port (default: 80)
    uint16_t max_uri_handlers;      ///< Maximum number of URI handlers (default: 16)
    bool cors_enabled;              ///< Enable CORS headers (default: true)
    bool lru_purge_enable;          ///< Enable LRU purging (default: true)
    uint32_t stack_size;            ///< HTTP server stack size (0 = default)
    uint16_t max_open_sockets;      ///< Maximum open sockets (0 = default)
} mcp_http_config_t;

/**
 * @brief Custom HTTP handler registration structure
 */
typedef struct {
    const char* uri;                ///< URI pattern
    httpd_method_t method;          ///< HTTP method
    esp_err_t (*handler)(httpd_req_t *req);  ///< Handler function
    void* user_ctx;                 ///< User context data
} mcp_http_handler_t;

/**
 * @brief Default HTTP server configuration
 */
#define MCP_HTTP_DEFAULT_CONFIG() {    \
    .port = 80,                        \
    .max_uri_handlers = 16,            \
    .cors_enabled = true,              \
    .lru_purge_enable = true,          \
    .stack_size = 0,                   \
    .max_open_sockets = 0              \
}

/**
 * @brief Start HTTP server with MCP framework configuration
 * 
 * @param config HTTP server configuration
 * @return httpd_handle_t Server handle on success, NULL on failure
 */
httpd_handle_t mcp_http_server_start(const mcp_http_config_t* config);

/**
 * @brief Stop HTTP server
 * 
 * @param server Server handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_http_server_stop(httpd_handle_t server);

/**
 * @brief Register MCP endpoints from configuration
 * 
 * This function automatically registers POST and OPTIONS handlers for all
 * routes defined in the MCP configuration.
 * 
 * @param server HTTP server handle
 * @param mcp_config MCP configuration containing routes
 * @param mcp_handler MCP request handler function
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_http_register_mcp_endpoints(httpd_handle_t server, 
                                          const mcp_config_t* mcp_config,
                                          esp_err_t (*mcp_handler)(httpd_req_t *req));

/**
 * @brief Register custom HTTP handlers
 * 
 * @param server HTTP server handle
 * @param handlers Array of handler configurations
 * @param handler_count Number of handlers to register
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_http_register_handlers(httpd_handle_t server,
                                     const mcp_http_handler_t* handlers,
                                     size_t handler_count);

/**
 * @brief Built-in CORS OPTIONS handler
 * 
 * This handler can be used for CORS preflight requests.
 * 
 * @param req HTTP request
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_http_cors_options_handler(httpd_req_t *req);

/**
 * @brief Built-in health check handler
 * 
 * Returns a simple JSON health status response.
 * 
 * @param req HTTP request
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_http_health_handler(httpd_req_t *req);

/**
 * @brief Set CORS headers on a response
 * 
 * @param req HTTP request
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_http_set_cors_headers(httpd_req_t *req);

// ========== Server-Sent Events (SSE) Support ==========

/**
 * @brief SSE event data structure
 */
typedef struct {
    const char* event;          ///< Event type (optional, can be NULL)
    const char* data;           ///< Event data
    const char* id;             ///< Event ID (optional, can be NULL)
    int retry;                  ///< Retry interval in milliseconds (0 = no retry field)
} mcp_sse_event_t;

/**
 * @brief SSE connection context (opaque structure)
 */
typedef struct mcp_sse_connection mcp_sse_connection_t;

/**
 * @brief Initialize SSE headers for a connection
 * 
 * This function sets the required SSE headers and sends the initial connection event.
 * 
 * @param req HTTP request (will be used for streaming)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_sse_init_connection(httpd_req_t *req);

/**
 * @brief Send SSE event to client
 * 
 * @param req HTTP request handle
 * @param event SSE event data
 * @return esp_err_t ESP_OK on success, ESP_FAIL if client disconnected
 */
esp_err_t mcp_sse_send_event(httpd_req_t *req, const mcp_sse_event_t* event);

/**
 * @brief Send simple SSE event with just event type and data
 * 
 * @param req HTTP request handle
 * @param event_type Event type (can be NULL)
 * @param data Event data
 * @return esp_err_t ESP_OK on success, ESP_FAIL if client disconnected
 */
esp_err_t mcp_sse_send_simple(httpd_req_t *req, const char* event_type, const char* data);

/**
 * @brief Finalize SSE connection (close stream)
 * 
 * @param req HTTP request handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_sse_close_connection(httpd_req_t *req);

/**
 * @brief Register SSE endpoint with session manager integration
 * 
 * @param server HTTP server handle
 * @param uri URI pattern for SSE endpoint (typically "/sse")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_http_register_sse_endpoint(httpd_handle_t server, const char* uri);

/**
 * @brief Built-in SSE demo handler
 * 
 * This demonstrates how to implement SSE endpoints. Applications can use this
 * as a template or extend it for their specific needs.
 * 
 * @param req HTTP request
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcp_http_sse_demo_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif // MCP_HTTP_SERVER_H