#include "mcp_pthread_pool.h"
#include "mcp_memory.h"
#include "mcp_wasm_runtime.h"
#include "mcp_session_manager.h"
#include "mcp_host_functions.h"
#include <esp_log.h>
#include <string.h>
#include <stdlib.h>
#include "wasm_export.h"
#include "mcp_http_server.h"
#include "esp_timer.h"

static const char *TAG = "mcp-pthread";

// Global pthread pool instance
static mcp_pthread_pool_t g_pool = {0};

// Largest request body accepted, and the size of the reply buffer a module writes into
#define MCP_MAX_REQUEST_BODY (16 * 1024)
#define MCP_RESPONSE_BUFFER_SIZE (8 * 1024)
// A single WASM call longer than this is terminated (tools like a Bluetooth scan
// legitimately take up to ~30s)
#define MCP_WASM_CALL_TIMEOUT_MS (60 * 1000)
// External references are now declared in the header file

// Forward declarations
static void *mcp_worker_thread(void *arg);
static void process_mcp_request(mcp_request_t *req);

esp_err_t mcp_pthread_pool_init(int pool_size)
{
    if (g_pool.initialized)
    {
        ESP_LOGW(TAG, "MCP pthread pool already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing MCP pthread pool with %d workers", pool_size);

    if (pool_size < 1)
    {
        pool_size = 1;
    }
    g_pool.pool_size = 0; // counts workers actually started
    g_pool.shutdown = false;

    // Create FreeRTOS queue for requests (queue size = pool_size * 2)
    g_pool.request_queue = xQueueCreate(pool_size * 2, sizeof(mcp_request_t *));
    if (!g_pool.request_queue)
    {
        ESP_LOGE(TAG, "Failed to create request queue");
        return ESP_FAIL;
    }

    // Allocate worker pthread array
    g_pool.workers = mcp_calloc(pool_size, sizeof(pthread_t));
    if (!g_pool.workers)
    {
        ESP_LOGE(TAG, "Failed to allocate worker array");
        vQueueDelete(g_pool.request_queue);
        return ESP_FAIL;
    }

    // Create worker pthreads with larger stack size
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384); // 16KB per worker

    for (int i = 0; i < pool_size; i++)
    {
        int *worker_id = mcp_malloc(sizeof(int));
        if (!worker_id)
        {
            ESP_LOGE(TAG, "Failed to allocate worker id %d", i);
            break;
        }
        *worker_id = i;

        int ret = pthread_create(&g_pool.workers[i], &attr, mcp_worker_thread, worker_id);
        if (ret != 0)
        {
            ESP_LOGE(TAG, "Failed to create worker thread %d: %d", i, ret);
            mcp_free(worker_id);
            break;
        }
        g_pool.pool_size = i + 1;

        ESP_LOGD(TAG, "Created worker pthread %d", i);
    }

    pthread_attr_destroy(&attr);

    if (g_pool.pool_size == 0)
    {
        ESP_LOGE(TAG, "No worker threads could be started");
        mcp_free(g_pool.workers);
        g_pool.workers = NULL;
        vQueueDelete(g_pool.request_queue);
        g_pool.request_queue = NULL;
        return ESP_FAIL;
    }
    if (g_pool.pool_size < pool_size)
    {
        ESP_LOGW(TAG, "Started %d of %d worker threads", g_pool.pool_size, pool_size);
    }

    g_pool.initialized = true;
    ESP_LOGI(TAG, "✅ MCP pthread pool initialized successfully");
    return ESP_OK;
}

esp_err_t mcp_pthread_pool_shutdown(void)
{
    if (!g_pool.initialized)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Shutting down MCP pthread pool");

    // Signal shutdown to all workers
    g_pool.shutdown = true;

    // Send shutdown signals to wake up waiting workers
    for (int i = 0; i < g_pool.pool_size; i++)
    {
        mcp_request_t *shutdown_req = NULL;
        xQueueSend(g_pool.request_queue, &shutdown_req, pdMS_TO_TICKS(1000));
    }

    // Wait for all workers to finish
    for (int i = 0; i < g_pool.pool_size; i++)
    {
        if (g_pool.workers[i])
        {
            pthread_join(g_pool.workers[i], NULL);
            ESP_LOGD(TAG, "Worker thread %d joined", i);
        }
    }

    // Cleanup resources
    if (g_pool.workers)
    {
        mcp_free(g_pool.workers);
        g_pool.workers = NULL;
    }

    if (g_pool.request_queue)
    {
        vQueueDelete(g_pool.request_queue);
        g_pool.request_queue = NULL;
    }

    g_pool.initialized = false;
    ESP_LOGI(TAG, "MCP pthread pool shutdown complete");
    return ESP_OK;
}

// Free a request and everything it owns. Does not touch http_req.
static void mcp_request_free(mcp_request_t *req)
{
    if (!req)
        return;
    mcp_free(req->response);
    mcp_free(req->request_body);
    mcp_free(req->uri);
    mcp_free(req->headers);
    mcp_free(req->response_headers);
    mcp_free(req);
}

// Copy the JSON-RPC id token (number or string) from the request so host-generated
// errors answer the right request. Falls back to null.
static void extract_request_id(const char *body, char *out, size_t out_size)
{
    snprintf(out, out_size, "null");
    const char *p = body ? strstr(body, "\"id\"") : NULL;
    if (!p)
        return;
    p += 4;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != ':')
        return;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    const char *end = p;
    if (*p == '"')
    {
        end = p + 1;
        while (*end && *end != '"')
        {
            if (*end == '\\' && end[1])
                end++;
            end++;
        }
        if (*end != '"')
            return;
        end++;
    }
    else
    {
        while (*end == '-' || (*end >= '0' && *end <= '9'))
            end++;
    }

    size_t len = end - p;
    if (len > 0 && len < out_size)
    {
        memcpy(out, p, len);
        out[len] = '\0';
    }
}

// Fill req->response with a JSON-RPC error that echoes the request's id.
// message must be a JSON-safe literal.
static void set_error_response(mcp_request_t *req, int code, const char *message)
{
    char id[64];
    extract_request_id(req->request_body, id, sizeof(id));
    snprintf(req->response, req->response_size,
             "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%s}",
             code, message, id);
}

// Send req->response (plus any headers the module set) on the async request,
// then complete it. Runs on the worker thread.
static void send_mcp_response(mcp_request_t *mcp_req)
{
    httpd_req_t *req = mcp_req->http_req;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // Apply response headers set by WASM module. httpd_resp_set_hdr stores
    // pointers, so the copies stay allocated until the response is sent.
#define MAX_RESPONSE_HEADERS 10
    char *header_buffers[MAX_RESPONSE_HEADERS * 2] = {0}; // name,value pairs
    int header_buffer_count = 0;

    if (mcp_req->response_headers && mcp_req->response_headers_length > 0)
    {
        ESP_LOGI(TAG, "Applying response headers: %s", mcp_req->response_headers);

        const char *pos = mcp_req->response_headers;
        while (*pos && header_buffer_count < MAX_RESPONSE_HEADERS * 2)
        {
            const char *colon = strchr(pos, ':');
            const char *line_end = strstr(pos, "\r\n");
            if (!colon || !line_end || colon > line_end)
                break;

            size_t name_len = colon - pos;
            const char *value = colon + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            size_t value_len = line_end - value;

            if (name_len > 0 && name_len < 128 && value_len < 256)
            {
                char *header_name = mcp_malloc(name_len + 1);
                char *header_value = mcp_malloc(value_len + 1);
                if (!header_name || !header_value)
                {
                    mcp_free(header_name);
                    mcp_free(header_value);
                    ESP_LOGE(TAG, "Failed to allocate response header");
                    break;
                }
                memcpy(header_name, pos, name_len);
                header_name[name_len] = '\0';
                memcpy(header_value, value, value_len);
                header_value[value_len] = '\0';
                header_buffers[header_buffer_count++] = header_name;
                header_buffers[header_buffer_count++] = header_value;

                httpd_resp_set_hdr(req, header_name, header_value);
                ESP_LOGI(TAG, "Applied header: %s = %s", header_name, header_value);
            }

            pos = line_end + 2;
        }
    }

    esp_err_t result;
    size_t len = strlen(mcp_req->response);
    if (len == 0)
    {
        // JSON-RPC notification: no response body (MCP Streamable HTTP expects 202)
        httpd_resp_set_status(req, "202 Accepted");
        result = httpd_resp_send(req, NULL, 0);
    }
    else
    {
        ESP_LOGI(TAG, "Sending MCP response: %s", mcp_req->response);
        result = httpd_resp_send(req, mcp_req->response, len);
    }

    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send MCP response: %s", esp_err_to_name(result));
    }

    for (int i = 0; i < header_buffer_count; i++)
    {
        mcp_free(header_buffers[i]);
    }

    mcp_http_async_complete(req);
    mcp_req->http_req = NULL;
}

static void wasm_watchdog_cb(void *arg)
{
    ESP_LOGE(TAG, "WASM call exceeded %d ms - terminating", MCP_WASM_CALL_TIMEOUT_MS);
    wasm_runtime_terminate((wasm_module_inst_t)arg);
}

static void *mcp_worker_thread(void *arg)
{
    int worker_id = *(int *)arg;
    mcp_free(arg); // Free the worker ID memory

    ESP_LOGI(TAG, "Worker pthread %d started, thread ID: %p", worker_id, (void *)pthread_self());

    // Initialize WAMR thread environment for this pthread
    if (!wasm_runtime_init_thread_env())
    {
        ESP_LOGE(TAG, "Worker %d: Failed to initialize WAMR thread environment", worker_id);
        return NULL;
    }
    ESP_LOGD(TAG, "Worker %d: WAMR thread environment initialized", worker_id);

    while (!g_pool.shutdown)
    {
        mcp_request_t *req = NULL;

        // Wait for request from queue (blocking)
        if (xQueueReceive(g_pool.request_queue, &req, portMAX_DELAY) == pdTRUE)
        {
            if (req == NULL)
            {
                // NULL request means shutdown signal
                ESP_LOGD(TAG, "Worker %d received shutdown signal", worker_id);
                break;
            }

            ESP_LOGD(TAG, "Worker %d processing MCP request to %s", worker_id, req->uri);

            // Process the MCP request in pthread context, reply, and release it.
            // The worker owns the request from here on.
            process_mcp_request(req);
            send_mcp_response(req);
            mcp_request_free(req);

            ESP_LOGD(TAG, "Worker %d completed request", worker_id);
        }
    }

    ESP_LOGI(TAG, "Worker pthread %d exiting", worker_id);

    // Cleanup WAMR thread environment
    wasm_runtime_destroy_thread_env();
    ESP_LOGD(TAG, "Worker %d: WAMR thread environment destroyed", worker_id);

    return NULL;
}

// Runs the request through the route's WASM module and leaves the reply (or a
// JSON-RPC error) in req->response. Every exit path releases the module mutex,
// frees WASM buffers and clears the per-thread request/session context.
static void process_mcp_request(mcp_request_t *req)
{
    ESP_LOGI(TAG, "Processing MCP request to %s: %s", req->uri, req->request_body);

    // Extract and set session context for this thread
    char session_id[MCP_SESSION_ID_LENGTH] = {0};
    bool has_session = mcp_session_extract_from_request(req->headers, req->uri, req->request_body, session_id);

    if (has_session)
    {
        mcp_session_set_current(session_id);
        ESP_LOGD(TAG, "Set session context for thread: %s", session_id);
    }
    else
    {
        ESP_LOGW(TAG, "No session ID found in request");
    }

    // Route matching ignores any query string (e.g. ?session_id=...)
    char route[128];
    snprintf(route, sizeof(route), "%.*s", (int)strcspn(req->uri, "?"), req->uri);

    // Find WASM module for this route
    const char *wasm_filename = mcp_wasm_find_file_for_route(route);
    if (!wasm_filename)
    {
        ESP_LOGW(TAG, "No WASM module configured for route: %s", route);
        set_error_response(req, -32601, "Route not configured");
        mcp_session_clear_current();
        return;
    }

    // Load WASM module into main thread runtime
    mcp_wasm_module_t *module = mcp_wasm_load_module_for_route(route, wasm_filename);
    if (!module)
    {
        ESP_LOGE(TAG, "Failed to load WASM module for route: %s", route);
        set_error_response(req, -32603, "Failed to load WASM module");
        mcp_session_clear_current();
        return;
    }

    mcp_wasm_dump_exports(module->wasm_module, wasm_filename);

    ESP_LOGI(TAG, "🔍 DEBUG: After load - module ptr: %p, wasm_module: %p, wasm_module_inst: %p",
             module, module->wasm_module, module->wasm_module_inst);
    ESP_LOGI(TAG, "🔍 DEBUG: Module has exec_env: %p", module->exec_env);

    pthread_mutex_lock(&module->module_mutex);

    uint64_t request_wasm_buffer = 0;
    uint64_t response_wasm_buffer = 0;
    void *request_native_ptr = NULL;
    void *response_native_ptr = NULL;
    const size_t response_size = MCP_RESPONSE_BUFFER_SIZE;

    // Look for MCP request handler function with retry mechanism for timing issues
    wasm_function_inst_t handle_request_func = NULL;
    int retry_count = 0;
    const int max_retries = 5;

    while (!handle_request_func && retry_count < max_retries)
    {
        handle_request_func = wasm_runtime_lookup_function(module->wasm_module_inst, "handle_mcp_request");
        ESP_LOGI(TAG, "🔍 DEBUG: Function lookup attempt %d returned: %p", retry_count + 1, handle_request_func);

        if (!handle_request_func)
        {
            retry_count++;
            if (retry_count < max_retries)
            {
                ESP_LOGW(TAG, "🔍 DEBUG: Function lookup failed, retrying in 10ms (attempt %d/%d)", retry_count, max_retries);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    if (!handle_request_func)
    {
        ESP_LOGE(TAG, "WASM module %s missing handle_mcp_request function after %d retries", wasm_filename, max_retries);
        set_error_response(req, -32603, "Invalid WASM module");
        goto unlock;
    }

    ESP_LOGI(TAG, "🔍 DEBUG: Request length: %d", req->request_len);
    request_wasm_buffer = wasm_runtime_module_malloc(module->wasm_module_inst, req->request_len + 1, &request_native_ptr);
    response_wasm_buffer = wasm_runtime_module_malloc(module->wasm_module_inst, response_size, &response_native_ptr);

    if (request_wasm_buffer == 0 || response_wasm_buffer == 0 || !request_native_ptr || !response_native_ptr)
    {
        ESP_LOGE(TAG, "Failed to allocate WASM memory (request %u bytes, response %u bytes)",
                 (unsigned)(req->request_len + 1), (unsigned)response_size);
        set_error_response(req, -32603, "WASM module out of memory");
        goto unlock;
    }

    ESP_LOGI(TAG, "🔍 DEBUG: Allocated WASM memory - request_buffer: %llu, response_buffer: %llu", request_wasm_buffer, response_wasm_buffer);

    // Copy request to WASM memory and clear the response buffer
    memcpy(request_native_ptr, req->request_body, req->request_len + 1);
    memset(response_native_ptr, 0, response_size);

    // Call WASM function: handle_mcp_request(request, response, max_len)
    uint32_t argv[3] = {(uint32_t)request_wasm_buffer, (uint32_t)response_wasm_buffer, response_size};

    // Arm the per-module watchdog (created once, reused for every call)
    if (!module->watchdog)
    {
        const esp_timer_create_args_t wd_args = {
            .callback = wasm_watchdog_cb,
            .arg = module->wasm_module_inst,
            .name = "wasm_watchdog",
        };
        esp_timer_handle_t wd = NULL;
        if (esp_timer_create(&wd_args, &wd) == ESP_OK)
        {
            module->watchdog = wd;
        }
    }

    // Set request context for host functions
    mcp_host_set_request_context(req);
    wasm_runtime_clear_exception(module->wasm_module_inst);
    if (module->watchdog)
    {
        esp_timer_start_once((esp_timer_handle_t)module->watchdog, (uint64_t)MCP_WASM_CALL_TIMEOUT_MS * 1000);
    }

    bool call_success = wasm_runtime_call_wasm(module->exec_env, handle_request_func, 3, argv);

    if (module->watchdog)
    {
        esp_timer_stop((esp_timer_handle_t)module->watchdog);
    }
    mcp_host_clear_request_context();

    int32_t wasm_return_value = (int32_t)argv[0];
    ESP_LOGI(TAG, "🔍 DEBUG: WASM call result: %s, returned %ld", call_success ? "SUCCESS" : "FAILED", (long)wasm_return_value);

    // this is here for debugging wasm func export table corruption after response is written to
    mcp_wasm_dump_exports(module->wasm_module, wasm_filename);
    mcp_memory_stats();

    if (!call_success)
    {
        const char *exception = wasm_runtime_get_exception(module->wasm_module_inst);
        ESP_LOGE(TAG, "WASM MCP handler failed: %s", exception ? exception : "Unknown error");
        set_error_response(req, -32603, "WASM handler failed");
        goto unlock;
    }

    // The module is not trusted to NUL-terminate inside its buffer
    ((char *)response_native_ptr)[response_size - 1] = '\0';

    if (wasm_return_value == -2)
    {
        // SDK convention: the reply did not fit in max_len
        ESP_LOGE(TAG, "WASM response truncated (buffer %u bytes)", (unsigned)response_size);
        set_error_response(req, -32603, "Response too large");
        goto unlock;
    }

    snprintf(req->response, req->response_size, "%s", (char *)response_native_ptr);
    ESP_LOGI(TAG, "✅ MCP request processed successfully: %s", req->response);

unlock:
    if (request_wasm_buffer)
        wasm_runtime_module_free(module->wasm_module_inst, request_wasm_buffer);
    if (response_wasm_buffer)
        wasm_runtime_module_free(module->wasm_module_inst, response_wasm_buffer);
    pthread_mutex_unlock(&module->module_mutex);
    mcp_session_clear_current();
}

// Send a JSON-RPC style error straight from the HTTP task (before a worker owns the request)
static esp_err_t send_http_error(httpd_req_t *req, const char *status, int code, const char *message)
{
    char body[160];
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":null}", code, message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// HTTP task: read the request, hand it to a worker, and return immediately.
// The worker replies on an async copy of the request (see send_mcp_response).
esp_err_t mcp_pthread_handler(httpd_req_t *req)
{
    if (!g_pool.initialized)
    {
        ESP_LOGE(TAG, "MCP pthread pool not initialized");
        return send_http_error(req, "503 Service Unavailable", -32603, "Service unavailable");
    }

    if (req->content_len == 0)
    {
        return send_http_error(req, "400 Bad Request", -32700, "Empty request body");
    }
    if (req->content_len > MCP_MAX_REQUEST_BODY)
    {
        ESP_LOGW(TAG, "Rejecting %u byte request (limit %d)", (unsigned)req->content_len, MCP_MAX_REQUEST_BODY);
        return send_http_error(req, "413 Content Too Large", -32600, "Request too large");
    }

    mcp_request_t *mcp_req = mcp_calloc(1, sizeof(mcp_request_t));
    if (!mcp_req)
    {
        return send_http_error(req, "500 Internal Server Error", -32603, "Memory allocation failed");
    }

    mcp_req->request_body = mcp_malloc(req->content_len + 1);
    mcp_req->response_size = MCP_RESPONSE_BUFFER_SIZE;
    mcp_req->response = mcp_malloc(mcp_req->response_size);

    size_t uri_len = strlen(req->uri);
    mcp_req->uri = mcp_malloc(uri_len + 1);

    if (!mcp_req->request_body || !mcp_req->response || !mcp_req->uri)
    {
        mcp_request_free(mcp_req);
        return send_http_error(req, "500 Internal Server Error", -32603, "Memory allocation failed");
    }
    memcpy(mcp_req->uri, req->uri, uri_len);
    mcp_req->uri[uri_len] = '\0';
    mcp_req->response[0] = '\0';

    // Read the whole body - it can arrive over several TCP segments
    size_t received = 0;
    int timeouts = 0;
    while (received < req->content_len)
    {
        int ret = httpd_req_recv(req, mcp_req->request_body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
        {
            continue;
        }
        if (ret <= 0)
        {
            ESP_LOGE(TAG, "Failed to receive request body (%u/%u bytes)", (unsigned)received, (unsigned)req->content_len);
            mcp_request_free(mcp_req);
            return send_http_error(req, "400 Bad Request", -32700, "Failed to read request");
        }
        received += ret;
    }
    mcp_req->request_body[received] = '\0';
    mcp_req->request_len = received;

    // Extract important headers for WASM access (limited set for security/size)
    char header_buffer[1024];
    char header_value[256];
    size_t header_len = 0;
    header_buffer[0] = '\0';

    const char *important_headers[] = {
        "MCP-Session-ID", // MCP session management
        "Mcp-Session-Id", // MCP spec format
        "Content-Type",   // Request content type
        "User-Agent",     // Client identification
        "Authorization",  // Security/auth tokens
        "X-Request-ID",   // Request tracing
        NULL};

    for (int i = 0; important_headers[i] != NULL && header_len < sizeof(header_buffer) - 100; i++)
    {
        if (httpd_req_get_hdr_value_str(req, important_headers[i], header_value, sizeof(header_value)) == ESP_OK)
        {
            int added = snprintf(header_buffer + header_len, sizeof(header_buffer) - header_len,
                                 "%s: %s\r\n", important_headers[i], header_value);
            if (added > 0 && header_len + added < sizeof(header_buffer))
            {
                header_len += added;
            }
        }
    }

    if (header_len > 0)
    {
        mcp_req->headers = mcp_malloc(header_len + 1);
        if (mcp_req->headers)
        {
            memcpy(mcp_req->headers, header_buffer, header_len + 1);
        }
    }

    // Detach from the HTTP task: the worker replies on the async copy
    if (httpd_req_async_handler_begin(req, &mcp_req->http_req) != ESP_OK)
    {
        mcp_request_free(mcp_req);
        return send_http_error(req, "500 Internal Server Error", -32603, "Memory allocation failed");
    }

    ESP_LOGI(TAG, "Queuing MCP request to %s", mcp_req->uri);

    if (xQueueSend(g_pool.request_queue, &mcp_req, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to queue MCP request - pool busy");
        httpd_req_t *async_req = mcp_req->http_req;
        send_http_error(async_req, "503 Service Unavailable", -32000, "Server busy, retry later");
        mcp_http_async_complete(async_req);
        mcp_request_free(mcp_req);
    }

    return ESP_OK;
}

esp_err_t mcp_pthread_pool_stats(int *active_workers, int *queue_size)
{
    if (!g_pool.initialized)
    {
        return ESP_FAIL;
    }

    *active_workers = g_pool.pool_size; // TODO: Track actual active count
    *queue_size = uxQueueMessagesWaiting(g_pool.request_queue);

    return ESP_OK;
}
