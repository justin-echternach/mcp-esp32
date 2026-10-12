#include "mcp_session_manager.h"
#include "mcp_http_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "MCP_SESSION";

// ========== Global Session Registry ==========

static mcp_session_t g_sessions[MCP_MAX_SESSIONS];
static pthread_mutex_t g_sessions_mutex = PTHREAD_MUTEX_INITIALIZER;
// One I/O lock per session slot. Serializes writes to that slot's SSE stream
// (keep-alive pings from its SSE task and events from worker threads) and keeps
// a stream from being completed while an event is being written. Per-slot so a
// slow client only stalls its own stream.
// Lock order: g_sse_io_mutex[slot], then g_sessions_mutex.
static pthread_mutex_t g_sse_io_mutex[MCP_MAX_SESSIONS] = {
    [0 ... MCP_MAX_SESSIONS - 1] = PTHREAD_MUTEX_INITIALIZER
};

static int session_slot(const char* session_id) {
    int slot = -1;
    pthread_mutex_lock(&g_sessions_mutex);
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (g_sessions[i].session_id[0] && strcmp(g_sessions[i].session_id, session_id) == 0) {
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_sessions_mutex);
    return slot;
}
static bool g_manager_initialized = false;
static mcp_session_config_t g_config = {
    .max_sessions = MCP_MAX_SESSIONS,
    .session_timeout_minutes = MCP_SESSION_TIMEOUT_MIN,
    .sse_keepalive_seconds = MCP_SSE_KEEPALIVE_SEC,
    .legacy_client_support = true
};

// ========== Thread-Local Storage ==========

static _Thread_local char tls_current_session_id[MCP_SESSION_ID_LENGTH] = {0};

void mcp_session_set_current(const char* session_id) {
    if (session_id) {
        strncpy(tls_current_session_id, session_id, MCP_SESSION_ID_LENGTH - 1);
        tls_current_session_id[MCP_SESSION_ID_LENGTH - 1] = '\0';
        ESP_LOGD(TAG, "Thread %p set session: %s", (void*)pthread_self(), session_id);
    } else {
        tls_current_session_id[0] = '\0';
    }
}

const char* mcp_session_get_current(void) {
    return tls_current_session_id[0] ? tls_current_session_id : NULL;
}

void mcp_session_clear_current(void) {
    tls_current_session_id[0] = '\0';
    ESP_LOGD(TAG, "Thread %p cleared session", (void*)pthread_self());
}

// ========== Utility Functions ==========

esp_err_t mcp_session_generate_uuid(char* uuid, size_t size) {
    if (!uuid || size < 37) {  // UUID v4 needs 36 chars + null terminator
        return ESP_ERR_INVALID_ARG;
    }
    
    // Generate random bytes
    uint8_t random_bytes[16];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    
    // Set version (4) and variant bits
    random_bytes[6] = (random_bytes[6] & 0x0F) | 0x40; // Version 4
    random_bytes[8] = (random_bytes[8] & 0x3F) | 0x80; // Variant bits
    
    // Format as UUID string
    snprintf(uuid, size,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        random_bytes[0], random_bytes[1], random_bytes[2], random_bytes[3],
        random_bytes[4], random_bytes[5],
        random_bytes[6], random_bytes[7],
        random_bytes[8], random_bytes[9],
        random_bytes[10], random_bytes[11], random_bytes[12], 
        random_bytes[13], random_bytes[14], random_bytes[15]);
    
    return ESP_OK;
}

const char* mcp_session_get_esp32_ip(void) {
    static char ip_str[16] = "0.0.0.0";
    
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            inet_ntop(AF_INET, &ip_info.ip, ip_str, sizeof(ip_str));
        }
    }
    
    return ip_str;
}

static uint64_t get_timestamp_ms(void) {
    return esp_timer_get_time() / 1000;  // Convert microseconds to milliseconds
}

// ========== Session Management ==========

esp_err_t mcp_session_manager_init(const mcp_session_config_t* config) {
    if (g_manager_initialized) {
        ESP_LOGW(TAG, "Session manager already initialized");
        return ESP_OK;
    }
    
    // Apply custom config if provided
    if (config) {
        memcpy(&g_config, config, sizeof(mcp_session_config_t));
    }
    
    // Initialize all sessions
    pthread_mutex_lock(&g_sessions_mutex);
    memset(g_sessions, 0, sizeof(g_sessions));
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        pthread_mutex_init(&g_sessions[i].lock, NULL);
    }
    pthread_mutex_unlock(&g_sessions_mutex);
    
    g_manager_initialized = true;
    ESP_LOGI(TAG, "Session manager initialized (max_sessions=%d, timeout=%d min)",
             g_config.max_sessions, g_config.session_timeout_minutes);
    
    return ESP_OK;
}

void mcp_session_manager_deinit(void) {
    if (!g_manager_initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_sessions_mutex);
    
    // Clean up all sessions
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (g_sessions[i].session_id[0]) {
            // Close any active SSE connections
            if (g_sessions[i].sse_active && g_sessions[i].sse_req) {
                mcp_sse_close_connection(g_sessions[i].sse_req);
            }
            pthread_mutex_destroy(&g_sessions[i].lock);
        }
    }
    
    memset(g_sessions, 0, sizeof(g_sessions));
    pthread_mutex_unlock(&g_sessions_mutex);
    
    g_manager_initialized = false;
    ESP_LOGI(TAG, "Session manager deinitialized");
}

static mcp_session_t* find_session_locked(const char* session_id) {
    if (!session_id) return NULL;
    
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (strcmp(g_sessions[i].session_id, session_id) == 0) {
            return &g_sessions[i];
        }
    }
    return NULL;
}

static mcp_session_t* allocate_session_locked(void) {
    // First, try to find an empty slot
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (g_sessions[i].session_id[0] == '\0') {
            return &g_sessions[i];
        }
    }
    
    // If no empty slots, find the oldest inactive session
    mcp_session_t* oldest = NULL;
    uint64_t oldest_activity = UINT64_MAX;
    
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (!g_sessions[i].sse_active && g_sessions[i].last_activity < oldest_activity) {
            oldest = &g_sessions[i];
            oldest_activity = g_sessions[i].last_activity;
        }
    }
    
    if (oldest) {
        // Clear the old session
        memset(oldest->session_id, 0, sizeof(oldest->session_id));
        oldest->sse_req = NULL;
        oldest->sse_active = false;
        return oldest;
    }
    
    return NULL;  // All sessions are active with SSE
}

const char* mcp_session_create(void) {
    static char new_session_id[MCP_SESSION_ID_LENGTH];
    
    // Generate new UUID
    if (mcp_session_generate_uuid(new_session_id, sizeof(new_session_id)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate UUID");
        return NULL;
    }
    
    pthread_mutex_lock(&g_sessions_mutex);
    
    // Allocate a session slot
    mcp_session_t* session = allocate_session_locked();
    if (!session) {
        pthread_mutex_unlock(&g_sessions_mutex);
        ESP_LOGE(TAG, "No available session slots");
        return NULL;
    }
    
    // Initialize the session
    strcpy(session->session_id, new_session_id);
    session->created_at = get_timestamp_ms();
    session->last_activity = session->created_at;
    session->sse_req = NULL;
    session->sse_active = false;
    session->socket_fd = -1;
    
    pthread_mutex_unlock(&g_sessions_mutex);
    
    ESP_LOGI(TAG, "Created new session: %s", new_session_id);
    return new_session_id;
}

const char* mcp_session_find_or_create(const char* session_id) {
    if (session_id && session_id[0]) {
        pthread_mutex_lock(&g_sessions_mutex);
        mcp_session_t* session = find_session_locked(session_id);
        pthread_mutex_unlock(&g_sessions_mutex);
        
        if (session) {
            mcp_session_touch(session_id);
            return session_id;
        }
    }
    
    // Session not found or no ID provided - create new
    return mcp_session_create();
}

void mcp_session_touch(const char* session_id) {
    if (!session_id) return;
    
    pthread_mutex_lock(&g_sessions_mutex);
    mcp_session_t* session = find_session_locked(session_id);
    if (session) {
        session->last_activity = get_timestamp_ms();
    }
    pthread_mutex_unlock(&g_sessions_mutex);
}

void mcp_session_cleanup_expired(void) {
    uint64_t now = get_timestamp_ms();
    uint64_t timeout_ms = g_config.session_timeout_minutes * 60 * 1000;
    int cleaned = 0;
    
    pthread_mutex_lock(&g_sessions_mutex);
    
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (g_sessions[i].session_id[0] && 
            (now - g_sessions[i].last_activity) > timeout_ms) {
            
            // Don't clean if SSE is active
            if (!g_sessions[i].sse_active) {
                ESP_LOGI(TAG, "Cleaning expired session: %s", g_sessions[i].session_id);
                memset(&g_sessions[i], 0, sizeof(mcp_session_t));
                cleaned++;
            }
        }
    }
    
    pthread_mutex_unlock(&g_sessions_mutex);
    
    if (cleaned > 0) {
        ESP_LOGI(TAG, "Cleaned %d expired sessions", cleaned);
    }
}

// ========== SSE Connection Management ==========

esp_err_t mcp_session_register_sse(const char* session_id, httpd_req_t* req) {
    if (!session_id || !req) {
        return ESP_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&g_sessions_mutex);
    
    mcp_session_t* session = find_session_locked(session_id);
    if (!session) {
        pthread_mutex_unlock(&g_sessions_mutex);
        ESP_LOGE(TAG, "Session not found for SSE: %s", session_id);
        return ESP_ERR_NOT_FOUND;
    }
    
    // An older stream for this session notices it is no longer current and
    // closes itself from its own task (see mcp_sse_task)
    if (session->sse_active && session->sse_req) {
        ESP_LOGW(TAG, "Replacing existing SSE connection for session: %s", session_id);
    }
    
    // Register new SSE connection
    session->sse_req = req;
    session->sse_server = req->handle;
    session->socket_fd = httpd_req_to_sockfd(req);
    session->sse_active = true;
    session->last_activity = get_timestamp_ms();
    session->last_ping = session->last_activity;
    
    pthread_mutex_unlock(&g_sessions_mutex);
    
    ESP_LOGI(TAG, "Registered SSE connection for session: %s (fd=%d)", 
             session_id, session->socket_fd);
    
    return ESP_OK;
}

void mcp_session_unregister_sse(const char* session_id) {
    if (!session_id) return;
    
    pthread_mutex_lock(&g_sessions_mutex);
    
    mcp_session_t* session = find_session_locked(session_id);
    if (session) {
        session->sse_req = NULL;
        session->sse_active = false;
        session->socket_fd = -1;
        ESP_LOGI(TAG, "Unregistered SSE connection for session: %s", session_id);
    }
    
    pthread_mutex_unlock(&g_sessions_mutex);
}

// Unregister only if req is still the session's active stream
static void unregister_sse_if_current(const char* session_id, httpd_req_t* req) {
    pthread_mutex_lock(&g_sessions_mutex);
    mcp_session_t* session = find_session_locked(session_id);
    if (session && session->sse_req == req) {
        session->sse_req = NULL;
        session->sse_active = false;
        session->socket_fd = -1;
        ESP_LOGI(TAG, "Unregistered SSE connection for session: %s", session_id);
    }
    pthread_mutex_unlock(&g_sessions_mutex);
}

static bool sse_is_current(const char* session_id, httpd_req_t* req) {
    pthread_mutex_lock(&g_sessions_mutex);
    mcp_session_t* session = find_session_locked(session_id);
    bool current = session && session->sse_active && session->sse_req == req;
    pthread_mutex_unlock(&g_sessions_mutex);
    return current;
}

bool mcp_session_has_sse(const char* session_id) {
    if (!session_id) return false;
    
    pthread_mutex_lock(&g_sessions_mutex);
    mcp_session_t* session = find_session_locked(session_id);
    bool has_sse = session && session->sse_active;
    pthread_mutex_unlock(&g_sessions_mutex);
    
    return has_sse;
}

// ========== Event Sending Functions ==========

esp_err_t mcp_session_send_event(const char* session_id, const char* event_type, const char* data) {
    if (!session_id || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int slot = session_slot(session_id);
    if (slot < 0) {
        ESP_LOGW(TAG, "Session not found for event: %s", session_id);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Hold the slot's I/O lock across lookup and send so the stream can't be
    // completed (freed) by its SSE task while we write to it
    pthread_mutex_lock(&g_sse_io_mutex[slot]);
    pthread_mutex_lock(&g_sessions_mutex);
    
    mcp_session_t* session = &g_sessions[slot];
    if (strcmp(session->session_id, session_id) != 0 || !session->sse_active || !session->sse_req) {
        pthread_mutex_unlock(&g_sessions_mutex);
        pthread_mutex_unlock(&g_sse_io_mutex[slot]);
        ESP_LOGW(TAG, "No SSE connection for session: %s", session_id);
        return ESP_ERR_INVALID_STATE;
    }
    
    httpd_req_t* req = session->sse_req;
    pthread_mutex_unlock(&g_sessions_mutex);
    
    esp_err_t ret = mcp_sse_send_simple(req, event_type, data);
    pthread_mutex_unlock(&g_sse_io_mutex[slot]);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send SSE event to session %s: %s", 
                 session_id, esp_err_to_name(ret));
        // Mark connection as inactive on error; its SSE task will clean up
        unregister_sse_if_current(session_id, req);
    } else {
        ESP_LOGD(TAG, "Sent SSE event to session %s: %s = %s", 
                 session_id, event_type ? event_type : "message", data);
    }
    
    return ret;
}

esp_err_t mcp_session_send_to_current(const char* event_type, const char* data) {
    const char* session_id = mcp_session_get_current();
    if (!session_id) {
        ESP_LOGW(TAG, "No current session in thread context");
        return ESP_ERR_NOT_FOUND;
    }
    
    return mcp_session_send_event(session_id, event_type, data);
}

int mcp_session_broadcast_event(const char* event_type, const char* data) {
    int sent_count = 0;
    
    pthread_mutex_lock(&g_sessions_mutex);
    
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (g_sessions[i].session_id[0] && g_sessions[i].sse_active) {
            // We need to send without holding the main mutex
            pthread_mutex_unlock(&g_sessions_mutex);
            
            esp_err_t ret = mcp_session_send_event(g_sessions[i].session_id, event_type, data);
            if (ret == ESP_OK) {
                sent_count++;
            }
            
            pthread_mutex_lock(&g_sessions_mutex);
        }
    }
    
    pthread_mutex_unlock(&g_sessions_mutex);
    
    ESP_LOGI(TAG, "Broadcast event to %d sessions", sent_count);
    return sent_count;
}

// ========== Session ID Extraction ==========

static bool extract_header_value(const char* headers, const char* header_name, char* value, size_t value_size) {
    if (!headers || !header_name || !value) return false;
    
    const char* header = strstr(headers, header_name);
    if (!header) return false;
    
    header += strlen(header_name);
    while (*header == ' ' || *header == ':') header++;
    
    size_t len = 0;
    while (header[len] && header[len] != '\r' && header[len] != '\n' && len < value_size - 1) {
        value[len] = header[len];
        len++;
    }
    value[len] = '\0';
    
    return len > 0;
}

static bool extract_url_param(const char* uri, const char* param_name, char* value, size_t value_size) {
    if (!uri || !param_name || !value) return false;
    
    // Look for ?param_name= or &param_name=
    char search_pattern[64];
    snprintf(search_pattern, sizeof(search_pattern), "%s=", param_name);
    
    const char* param = strstr(uri, search_pattern);
    if (!param) return false;
    
    // Make sure it's a parameter start
    if (param > uri && *(param - 1) != '?' && *(param - 1) != '&') {
        return false;
    }
    
    param += strlen(search_pattern);
    
    size_t len = 0;
    while (param[len] && param[len] != '&' && param[len] != ' ' && len < value_size - 1) {
        value[len] = param[len];
        len++;
    }
    value[len] = '\0';
    
    return len > 0;
}

bool mcp_session_extract_id(httpd_req_t* req, char* session_id, bool is_sse) {
    if (!req || !session_id) return false;
    
    // For SSE requests, check URL parameter first (legacy support)
    if (is_sse && g_config.legacy_client_support) {
        if (extract_url_param(req->uri, "client_id", session_id, MCP_SESSION_ID_LENGTH)) {
            ESP_LOGD(TAG, "Extracted session ID from URL: %s", session_id);
            return true;
        }
    }
    
    // Check MCP-Session-ID header
    char header_value[MCP_SESSION_ID_LENGTH];
    int header_len = httpd_req_get_hdr_value_len(req, "MCP-Session-ID");
    if (header_len > 0 && header_len < MCP_SESSION_ID_LENGTH) {
        if (httpd_req_get_hdr_value_str(req, "MCP-Session-ID", header_value, sizeof(header_value)) == ESP_OK) {
            strcpy(session_id, header_value);
            ESP_LOGD(TAG, "Extracted session ID from header: %s", session_id);
            return true;
        }
    }
    
    // For non-SSE requests, also check URL
    if (!is_sse && extract_url_param(req->uri, "session_id", session_id, MCP_SESSION_ID_LENGTH)) {
        ESP_LOGD(TAG, "Extracted session ID from URL: %s", session_id);
        return true;
    }
    
    return false;
}

bool mcp_session_extract_from_request(const char* headers, const char* uri, 
                                      const char* body, char* session_id) {
    if (!session_id) return false;
    
    // Check headers for MCP-Session-ID
    if (headers && extract_header_value(headers, "MCP-Session-ID", session_id, MCP_SESSION_ID_LENGTH)) {
        return true;
    }
    
    // Check URL parameters
    if (uri) {
        if (extract_url_param(uri, "client_id", session_id, MCP_SESSION_ID_LENGTH)) {
            return true;
        }
        if (extract_url_param(uri, "session_id", session_id, MCP_SESSION_ID_LENGTH)) {
            return true;
        }
    }
    
    // TODO: Check JSON body for session_id field
    // This would require JSON parsing which we want to keep lightweight
    
    return false;
}

// ========== Utility Functions ==========

esp_err_t mcp_session_get_stats(int* active_sessions, int* sse_connections) {
    if (!active_sessions || !sse_connections) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *active_sessions = 0;
    *sse_connections = 0;
    
    pthread_mutex_lock(&g_sessions_mutex);
    
    for (int i = 0; i < MCP_MAX_SESSIONS; i++) {
        if (g_sessions[i].session_id[0]) {
            (*active_sessions)++;
            if (g_sessions[i].sse_active) {
                (*sse_connections)++;
            }
        }
    }
    
    pthread_mutex_unlock(&g_sessions_mutex);
    
    return ESP_OK;
}

// ========== SSE Handler ==========


typedef struct {
    httpd_req_t* req;   // async copy, owned by the task
    char session_id[MCP_SESSION_ID_LENGTH];
} mcp_sse_task_ctx_t;

// One task per open SSE stream: sends keep-alive pings until the client goes
// away or the session gets a newer stream, then closes and completes the request.
static void mcp_sse_task(void* arg) {
    mcp_sse_task_ctx_t* ctx = (mcp_sse_task_ctx_t*)arg;
    httpd_req_t* req = ctx->req;
    int slot = session_slot(ctx->session_id);
    pthread_mutex_t* io_lock = &g_sse_io_mutex[slot < 0 ? 0 : slot];
    
    uint64_t last_ping = get_timestamp_ms();
    int ping_interval_ms = g_config.sse_keepalive_seconds * 1000;
    
    while (sse_is_current(ctx->session_id, req)) {
        uint64_t now = get_timestamp_ms();
        
        if ((now - last_ping) >= ping_interval_ms) {
            char ping_data[64];
            snprintf(ping_data, sizeof(ping_data), "{\"timestamp\":%llu}", now);
            pthread_mutex_lock(io_lock);
            esp_err_t ret = mcp_sse_send_simple(req, "ping", ping_data);
            pthread_mutex_unlock(io_lock);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "SSE ping failed, closing connection");
                break;
            }
            last_ping = now;
            mcp_session_touch(ctx->session_id);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    unregister_sse_if_current(ctx->session_id, req);
    
    // Wait for any in-flight event write, then close and release the request
    pthread_mutex_lock(io_lock);
    mcp_sse_close_connection(req);
    mcp_http_async_complete(req);
    pthread_mutex_unlock(io_lock);
    
    ESP_LOGI(TAG, "SSE stream finished for session: %s", ctx->session_id);
    free(ctx);
    vTaskDelete(NULL);
}

esp_err_t mcp_session_sse_handler(httpd_req_t *req) {
    char session_id[MCP_SESSION_ID_LENGTH];
    
    // Extract session ID from request
    if (!mcp_session_extract_id(req, session_id, true)) {
        ESP_LOGE(TAG, "SSE connection without session ID");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing session ID");
    }
    
    // Find or create session
    const char* valid_session = mcp_session_find_or_create(session_id);
    if (!valid_session) {
        ESP_LOGE(TAG, "Failed to find/create session for SSE");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Session error");
    }
    
    mcp_sse_task_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    strlcpy(ctx->session_id, valid_session, sizeof(ctx->session_id));
    
    // Initialize SSE connection (sends headers and the "connected" event)
    esp_err_t ret = mcp_sse_init_connection(req);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SSE connection");
        free(ctx);
        return ret;
    }
    
    // Hand the stream to its own task so the HTTP server task stays free
    if (httpd_req_async_handler_begin(req, &ctx->req) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to detach SSE request");
        free(ctx);
        return ESP_FAIL;
    }
    
    ret = mcp_session_register_sse(ctx->session_id, ctx->req);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register SSE with session");
        mcp_sse_close_connection(ctx->req);
        mcp_http_async_complete(ctx->req);
        free(ctx);
        return ESP_OK;
    }
    
    if (xTaskCreate(mcp_sse_task, "mcp_sse", 6144, ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start SSE task");
        unregister_sse_if_current(ctx->session_id, ctx->req);
        mcp_sse_close_connection(ctx->req);
        mcp_http_async_complete(ctx->req);
        free(ctx);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "SSE stream started for session: %s", ctx->session_id);
    return ESP_OK;
}
