#ifndef MCP_SESSION_MANAGER_H
#define MCP_SESSION_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== Configuration Constants ==========

#define MCP_SESSION_ID_LENGTH 64        // UUID string length
#define MCP_MAX_SESSIONS 16             // Maximum concurrent sessions
#define MCP_SESSION_TIMEOUT_MIN 30      // Session timeout in minutes
#define MCP_SSE_KEEPALIVE_SEC 30        // SSE ping interval in seconds

// ========== Session Data Structures ==========

/**
 * @brief MCP session structure
 * 
 * Represents a single MCP client session with associated SSE connection
 */
typedef struct {
    char session_id[MCP_SESSION_ID_LENGTH];     // UUID string
    httpd_req_t* sse_req;                       // Active SSE connection (NULL if not connected)
    httpd_handle_t sse_server;                  // Server handle for SSE
    int socket_fd;                              // Socket for connection validation
    bool sse_active;                            // SSE connection state
    uint64_t created_at;                        // Session creation timestamp (ms)
    uint64_t last_activity;                     // Last request/SSE activity (ms)
    uint64_t last_ping;                         // Last SSE keep-alive ping (ms)
    pthread_mutex_t lock;                       // Per-session mutex for thread safety
} mcp_session_t;

/**
 * @brief Session manager configuration
 */
typedef struct {
    int max_sessions;                           // Maximum concurrent sessions
    int session_timeout_minutes;                // Session expiry time
    int sse_keepalive_seconds;                  // SSE ping interval
    bool legacy_client_support;                 // Support URL-based client_id
} mcp_session_config_t;

// ========== Thread-Local Storage ==========

/**
 * @brief Set current session ID for this thread
 * 
 * @param session_id Session ID to set (will be copied)
 */
void mcp_session_set_current(const char* session_id);

/**
 * @brief Get current session ID for this thread
 * 
 * @return Current session ID or NULL if not set
 */
const char* mcp_session_get_current(void);

/**
 * @brief Clear current session ID for this thread
 */
void mcp_session_clear_current(void);

// ========== Session Management Functions ==========

/**
 * @brief Initialize the session manager
 * 
 * @param config Configuration parameters (can be NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t mcp_session_manager_init(const mcp_session_config_t* config);

/**
 * @brief Deinitialize the session manager
 */
void mcp_session_manager_deinit(void);

/**
 * @brief Create a new session with generated UUID
 * 
 * @return Session ID string (static buffer) or NULL on failure
 */
const char* mcp_session_create(void);

/**
 * @brief Find existing session or create new one
 * 
 * @param session_id Session ID to find (or NULL to create new)
 * @return Session ID string (static buffer) or NULL on failure
 */
const char* mcp_session_find_or_create(const char* session_id);

/**
 * @brief Update session last activity timestamp
 * 
 * @param session_id Session ID to update
 */
void mcp_session_touch(const char* session_id);

/**
 * @brief Remove expired sessions
 * 
 * Should be called periodically to clean up old sessions
 */
void mcp_session_cleanup_expired(void);

// ========== SSE Connection Management ==========

/**
 * @brief Register SSE connection with session
 * 
 * @param session_id Session ID
 * @param req HTTP request handle for SSE connection
 * @return ESP_OK on success
 */
esp_err_t mcp_session_register_sse(const char* session_id, httpd_req_t* req);

/**
 * @brief Unregister SSE connection from session
 * 
 * @param session_id Session ID
 */
void mcp_session_unregister_sse(const char* session_id);

/**
 * @brief Check if session has active SSE connection
 * 
 * @param session_id Session ID
 * @return true if SSE is connected
 */
bool mcp_session_has_sse(const char* session_id);

// ========== Event Sending Functions ==========

/**
 * @brief Send SSE event to specific session
 * 
 * @param session_id Target session ID
 * @param event_type Event type (can be NULL)
 * @param data Event data
 * @return ESP_OK on success
 */
esp_err_t mcp_session_send_event(const char* session_id, const char* event_type, const char* data);

/**
 * @brief Send SSE event to current thread's session
 * 
 * @param event_type Event type (can be NULL)
 * @param data Event data
 * @return ESP_OK on success
 */
esp_err_t mcp_session_send_to_current(const char* event_type, const char* data);

/**
 * @brief Send SSE event to all active sessions
 * 
 * @param event_type Event type (can be NULL)
 * @param data Event data
 * @return Number of sessions successfully sent to
 */
int mcp_session_broadcast_event(const char* event_type, const char* data);

// ========== Session ID Extraction Utilities ==========

/**
 * @brief Extract session ID from HTTP request
 * 
 * Checks in order:
 * 1. MCP-Session-ID header
 * 2. client_id URL parameter
 * 3. Session ID in JSON body
 * 
 * @param req HTTP request
 * @param session_id Buffer to store extracted ID (min MCP_SESSION_ID_LENGTH)
 * @param is_sse Set to true if this is an SSE request
 * @return true if session ID found
 */
bool mcp_session_extract_id(httpd_req_t* req, char* session_id, bool is_sse);

/**
 * @brief Extract session ID from MCP request structure
 * 
 * Used by pthread pool when processing MCP requests
 * 
 * @param headers Request headers (can be NULL)
 * @param uri Request URI
 * @param body Request body (can be NULL)
 * @param session_id Buffer to store extracted ID (min MCP_SESSION_ID_LENGTH)
 * @return true if session ID found
 */
bool mcp_session_extract_from_request(const char* headers, const char* uri, 
                                      const char* body, char* session_id);

// ========== Utility Functions ==========

/**
 * @brief Get session statistics
 * 
 * @param active_sessions Number of active sessions (output)
 * @param sse_connections Number of SSE connections (output)
 * @return ESP_OK on success
 */
esp_err_t mcp_session_get_stats(int* active_sessions, int* sse_connections);

/**
 * @brief Generate UUID v4 string
 * 
 * @param uuid Buffer to store UUID (min 37 bytes for standard format)
 * @param size Buffer size
 * @return ESP_OK on success
 */
esp_err_t mcp_session_generate_uuid(char* uuid, size_t size);

/**
 * @brief Get ESP32 IP address for SSE URL generation
 * 
 * @return IP address string (static buffer) or "0.0.0.0" if not connected
 */
const char* mcp_session_get_esp32_ip(void);

// ========== SSE Handler Integration ==========

/**
 * @brief HTTP handler for SSE connections
 * 
 * This should be registered as the handler for /sse endpoint
 * 
 * @param req HTTP request
 * @return ESP_OK on success
 */
esp_err_t mcp_session_sse_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif // MCP_SESSION_MANAGER_H