#include "mcp_pthread_handler.h"
#include <esp_log.h>
#include <string.h>
#include <stdlib.h>
#include "wasm_export.h"

static const char *TAG = "mcp-pthread";

// Global pthread pool instance
static mcp_pthread_pool_t g_pool = {0};

// External references are now declared in the header file

// Forward declarations
static void* mcp_worker_thread(void* arg);
static esp_err_t process_mcp_request(mcp_request_t* req);

esp_err_t mcp_pthread_pool_init(int pool_size) {
    if (g_pool.initialized) {
        ESP_LOGW(TAG, "MCP pthread pool already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing MCP pthread pool with %d workers", pool_size);
    
    g_pool.pool_size = pool_size;
    g_pool.shutdown = false;
    
    // Create FreeRTOS queue for requests (queue size = pool_size * 2)
    g_pool.request_queue = xQueueCreate(pool_size * 2, sizeof(mcp_request_t*));
    if (!g_pool.request_queue) {
        ESP_LOGE(TAG, "Failed to create request queue");
        return ESP_FAIL;
    }
    
    // Allocate worker pthread array
    g_pool.workers = calloc(pool_size, sizeof(pthread_t));
    if (!g_pool.workers) {
        ESP_LOGE(TAG, "Failed to allocate worker array");
        vQueueDelete(g_pool.request_queue);
        return ESP_FAIL;
    }
    
    // Create worker pthreads with larger stack size
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32768);  // 64KB stack
    
    for (int i = 0; i < pool_size; i++) {
        int* worker_id = malloc(sizeof(int));
        *worker_id = i;
        
        int ret = pthread_create(&g_pool.workers[i], &attr, mcp_worker_thread, worker_id);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to create worker thread %d: %d", i, ret);
            g_pool.shutdown = true;
            mcp_pthread_pool_shutdown();
            pthread_attr_destroy(&attr);
            return ESP_FAIL;
        }
        
        ESP_LOGD(TAG, "Created worker pthread %d", i);
    }
    
    pthread_attr_destroy(&attr);
    
    g_pool.initialized = true;
    ESP_LOGI(TAG, "✅ MCP pthread pool initialized successfully");
    return ESP_OK;
}

esp_err_t mcp_pthread_pool_shutdown(void) {
    if (!g_pool.initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Shutting down MCP pthread pool");
    
    // Signal shutdown to all workers
    g_pool.shutdown = true;
    
    // Send shutdown signals to wake up waiting workers
    for (int i = 0; i < g_pool.pool_size; i++) {
        mcp_request_t* shutdown_req = NULL;
        xQueueSend(g_pool.request_queue, &shutdown_req, 0);
    }
    
    // Wait for all workers to finish
    for (int i = 0; i < g_pool.pool_size; i++) {
        if (g_pool.workers[i]) {
            pthread_join(g_pool.workers[i], NULL);
            ESP_LOGD(TAG, "Worker thread %d joined", i);
        }
    }
    
    // Cleanup resources
    if (g_pool.workers) {
        free(g_pool.workers);
        g_pool.workers = NULL;
    }
    
    if (g_pool.request_queue) {
        vQueueDelete(g_pool.request_queue);
        g_pool.request_queue = NULL;
    }
    
    g_pool.initialized = false;
    ESP_LOGI(TAG, "MCP pthread pool shutdown complete");
    return ESP_OK;
}

static void* mcp_worker_thread(void* arg) {
    int worker_id = *(int*)arg;
    free(arg);  // Free the worker ID memory
    
    ESP_LOGI(TAG, "Worker pthread %d started, thread ID: %p", worker_id, (void*)pthread_self());
    
    // Initialize WAMR thread environment for this pthread
    if (!wasm_runtime_init_thread_env()) {
        ESP_LOGE(TAG, "Worker %d: Failed to initialize WAMR thread environment", worker_id);
        return NULL;
    }
    ESP_LOGD(TAG, "Worker %d: WAMR thread environment initialized", worker_id);
    
    while (!g_pool.shutdown) {
        mcp_request_t* req = NULL;
        
        // Wait for request from queue (blocking)
        if (xQueueReceive(g_pool.request_queue, &req, portMAX_DELAY) == pdTRUE) {
            if (req == NULL) {
                // NULL request means shutdown signal
                ESP_LOGD(TAG, "Worker %d received shutdown signal", worker_id);
                break;
            }
            
            ESP_LOGD(TAG, "Worker %d processing MCP request to %s", worker_id, req->uri);
            
            // Process the MCP request in pthread context
            req->result = process_mcp_request(req);
            
            // Signal completion to HTTP handler
            pthread_mutex_lock(&req->completion_mutex);
            req->completed = true;
            pthread_cond_signal(&req->completion_cond);
            pthread_mutex_unlock(&req->completion_mutex);
            
            ESP_LOGD(TAG, "Worker %d completed request", worker_id);
        }
    }
    
    ESP_LOGI(TAG, "Worker pthread %d exiting", worker_id);
    
    // Cleanup WAMR thread environment
    wasm_runtime_destroy_thread_env();
    ESP_LOGD(TAG, "Worker %d: WAMR thread environment destroyed", worker_id);
    
    return NULL;
}

static esp_err_t process_mcp_request(mcp_request_t* req) {
    ESP_LOGI(TAG, "Processing MCP request to %s: %s", req->uri, req->request_body);
    
    // Find WASM module for this route
    const char* wasm_filename = find_wasm_for_route(req->uri);
    if (!wasm_filename) {
        ESP_LOGW(TAG, "No WASM module configured for route: %s", req->uri);
        snprintf(req->response, sizeof(req->response),
                 "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-1,\"message\":\"Route not configured\"},\"id\":1}");
        return ESP_FAIL;
    }
    
    // Load MCP WASM into the existing main thread runtime (no separate runtime)
    ESP_LOGI(TAG, "🔧 Loading MCP WASM into main thread runtime from pthread context");
    ESP_LOGI(TAG, "🔍 DEBUG: Current thread ID: %p", (void*)pthread_self());
    
    // Load WASM module into main thread runtime
    mcp_wasm_module_t* module = load_mcp_wasm_module(req->uri, wasm_filename);
    if (!module) {
        ESP_LOGE(TAG, "Failed to load WASM module for route: %s", req->uri);
        snprintf(req->response, sizeof(req->response), 
                 "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-1,\"message\":\"Failed to load WASM module\"},\"id\":1}");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "🔍 DEBUG: After load - module ptr: %p, wasm_module: %p, wasm_module_inst: %p", 
             module, module->wasm_module, module->wasm_module_inst);
    ESP_LOGI(TAG, "🔍 DEBUG: Module has exec_env: %p", module->exec_env);
    
    // Look for MCP request handler function
    ESP_LOGI(TAG, "🔍 DEBUG: About to lookup function in pthread context");
    wasm_function_inst_t handle_request_func = wasm_runtime_lookup_function(module->wasm_module_inst, "handle_mcp_request");
    ESP_LOGI(TAG, "🔍 DEBUG: Function lookup returned: %p", handle_request_func);
    
    if (!handle_request_func) {
        ESP_LOGE(TAG, "WASM module %s missing handle_mcp_request function", wasm_filename);
        ESP_LOGE(TAG, "🔍 DEBUG: Lookup failed - module_inst still: %p", module->wasm_module_inst);
        snprintf(req->response, sizeof(req->response),
                 "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-1,\"message\":\"Invalid WASM module\"},\"id\":1}");
        return ESP_FAIL;
    }
    
    // Allocate WASM memory with manual offsets to stay within 64KB limit
    char* request_native_ptr;
    char* response_native_ptr;
    
    // Use fixed offsets within the first 64KB of WASM memory
    uint64_t request_wasm_buffer = 1024;  // Start at 1KB offset
    uint64_t response_wasm_buffer = 2048; // Start at 2KB offset
    size_t response_size = 512;  // Small response buffer
    
    // Get native pointers from WASM offsets
    request_native_ptr = (char*)wasm_runtime_addr_app_to_native(module->wasm_module_inst, request_wasm_buffer);
    response_native_ptr = (char*)wasm_runtime_addr_app_to_native(module->wasm_module_inst, response_wasm_buffer);
    
    // Validate we got valid pointers
    if (!request_native_ptr || !response_native_ptr) {
        ESP_LOGE(TAG, "Failed to get WASM memory pointers");
        snprintf(req->response, sizeof(req->response),
                 "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-1,\"message\":\"WASM memory mapping failed\"},\"id\":1}");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "🔍 DEBUG: Allocated WASM memory - request_buffer: %llu, response_buffer: %llu", request_wasm_buffer, response_wasm_buffer);
    ESP_LOGI(TAG, "🔍 DEBUG: Native pointers - request_native_ptr: %p, response_native_ptr: %p", request_native_ptr, response_native_ptr);
    
    // Copy request to WASM memory (using native pointers directly)
    strcpy(request_native_ptr, req->request_body);
    // Clear response buffer
    memset(response_native_ptr, 0, response_size);
    
    // Call WASM function exactly like basic sample: handle_mcp_request(request, response, max_len)
    uint32_t argv[3] = {(uint32_t)request_wasm_buffer, (uint32_t)response_wasm_buffer, response_size};
    
    ESP_LOGI(TAG, "🔍 DEBUG: About to call WASM function with argv: [%u, %u, %u]", argv[0], argv[1], argv[2]);
    ESP_LOGI(TAG, "🔍 DEBUG: Request body in WASM memory: '%.100s'", request_native_ptr);
    ESP_LOGI(TAG, "🔍 DEBUG: WASM memory bounds: request ends at %u, response ends at %u, module has 65536 bytes", 
             (uint32_t)(request_wasm_buffer + req->request_len + 1), (uint32_t)(response_wasm_buffer + response_size));
    
    // Clear any previous exceptions
    wasm_runtime_clear_exception(module->wasm_module_inst);
    
    bool call_success = wasm_runtime_call_wasm(module->exec_env, handle_request_func, 3, argv);
    
    ESP_LOGI(TAG, "🔍 DEBUG: WASM call result: %s", call_success ? "SUCCESS" : "FAILED");
    
    // Get detailed exception info immediately
    const char* exception = wasm_runtime_get_exception(module->wasm_module_inst);
    if (exception) {
        ESP_LOGE(TAG, "🔍 DEBUG: Detailed exception: %s", exception);
    }
    
    // Copy response from WASM memory (using native pointer directly)
    // Ensure we don't overflow the response buffer
    strncpy(req->response, response_native_ptr, sizeof(req->response) - 1);
    req->response[sizeof(req->response) - 1] = '\0';
    
    // No need to free WASM memory - using fixed offsets
    
    if (!call_success) {
        const char* exception = wasm_runtime_get_exception(module->wasm_module_inst);
        ESP_LOGE(TAG, "WASM MCP handler failed: %s", exception ? exception : "Unknown error");
        snprintf(req->response, sizeof(req->response),
                 "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-1,\"message\":\"WASM handler failed\"},\"id\":1}");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ MCP request processed successfully in main thread runtime: %s", req->response);
    
    return ESP_OK;
}

esp_err_t mcp_pthread_handler(httpd_req_t *req) {
    if (!g_pool.initialized) {
        ESP_LOGE(TAG, "MCP pthread pool not initialized");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Service unavailable");
        return ESP_FAIL;
    }
    
    // Read request body
    char* content = malloc(req->content_len + 1);
    if (!content) {
        ESP_LOGE(TAG, "Failed to allocate request buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    int ret = httpd_req_recv(req, content, req->content_len);
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to receive request body");
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    // Create MCP request structure
    mcp_request_t* mcp_req = malloc(sizeof(mcp_request_t));
    if (!mcp_req) {
        ESP_LOGE(TAG, "Failed to allocate MCP request");
        free(content);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    // Initialize request structure
    mcp_req->request_body = content;
    mcp_req->request_len = ret;
    mcp_req->uri = strdup(req->uri);
    mcp_req->completed = false;
    mcp_req->result = ESP_OK;
    pthread_mutex_init(&mcp_req->completion_mutex, NULL);
    pthread_cond_init(&mcp_req->completion_cond, NULL);
    
    ESP_LOGI(TAG, "Queuing MCP request to %s", req->uri);
    
    // Send request to worker pthread
    if (xQueueSend(g_pool.request_queue, &mcp_req, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to queue MCP request - pool busy");
        
        // Cleanup
        free(mcp_req->request_body);
        free(mcp_req->uri);
        pthread_mutex_destroy(&mcp_req->completion_mutex);
        pthread_cond_destroy(&mcp_req->completion_cond);
        free(mcp_req);
        
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Service busy");
        return ESP_FAIL;
    }
    
    // Wait for completion (with timeout)
    pthread_mutex_lock(&mcp_req->completion_mutex);
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 30;  // 30 second timeout
    
    int wait_result = 0;
    while (!mcp_req->completed && wait_result == 0) {
        wait_result = pthread_cond_timedwait(&mcp_req->completion_cond, 
                                           &mcp_req->completion_mutex, &timeout);
    }
    pthread_mutex_unlock(&mcp_req->completion_mutex);
    
    if (wait_result != 0 || !mcp_req->completed) {
        ESP_LOGE(TAG, "MCP request timeout or failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Request timeout");
    } else {
        // Send response
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_send(req, mcp_req->response, strlen(mcp_req->response));
        
        ESP_LOGI(TAG, "MCP response sent: %s", mcp_req->response);
    }
    
    // Cleanup request structure
    free(mcp_req->request_body);
    free(mcp_req->uri);
    pthread_mutex_destroy(&mcp_req->completion_mutex);
    pthread_cond_destroy(&mcp_req->completion_cond);
    free(mcp_req);
    
    return ESP_OK;
}

esp_err_t mcp_pthread_pool_stats(int* active_workers, int* queue_size) {
    if (!g_pool.initialized) {
        return ESP_FAIL;
    }
    
    *active_workers = g_pool.pool_size;  // TODO: Track actual active count
    *queue_size = uxQueueMessagesWaiting(g_pool.request_queue);
    
    return ESP_OK;
}