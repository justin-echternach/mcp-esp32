#include "mcp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "mcp_http_server";

httpd_handle_t mcp_http_server_start(const mcp_http_config_t* config) {
    if (!config) {
        ESP_LOGE(TAG, "Invalid HTTP server configuration");
        return NULL;
    }

    httpd_handle_t server = NULL;
    httpd_config_t esp_config = HTTPD_DEFAULT_CONFIG();
    
    // Apply MCP configuration
    esp_config.server_port = config->port;
    esp_config.max_uri_handlers = config->max_uri_handlers;
    esp_config.lru_purge_enable = config->lru_purge_enable;
    
    if (config->stack_size > 0) {
        esp_config.stack_size = config->stack_size;
    }
    
    if (config->max_open_sockets > 0) {
        esp_config.max_open_sockets = config->max_open_sockets;
    }
    
    ESP_LOGI(TAG, "Starting HTTP server on port %d", esp_config.server_port);
    ESP_LOGI(TAG, "Max URI handlers: %d", esp_config.max_uri_handlers);
    ESP_LOGI(TAG, "CORS enabled: %s", config->cors_enabled ? "true" : "false");
    
    esp_err_t ret = httpd_start(&server, &esp_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return NULL;
    }
    
    ESP_LOGI(TAG, "HTTP server started successfully");
    return server;
}

esp_err_t mcp_http_server_stop(httpd_handle_t server) {
    if (!server) {
        ESP_LOGW(TAG, "Server handle is NULL, nothing to stop");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Stopping HTTP server");
    esp_err_t ret = httpd_stop(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "HTTP server stopped successfully");
    }
    
    return ret;
}

esp_err_t mcp_http_register_mcp_endpoints(httpd_handle_t server, 
                                          const mcp_config_t* mcp_config,
                                          esp_err_t (*mcp_handler)(httpd_req_t *req)) {
    if (!server) {
        ESP_LOGE(TAG, "Server handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!mcp_config) {
        ESP_LOGE(TAG, "MCP configuration is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!mcp_handler) {
        ESP_LOGE(TAG, "MCP handler function is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Registering %d MCP endpoints...", mcp_config->route_count);
    
    for (int i = 0; i < mcp_config->route_count; i++) {
        const mcp_route_config_t* route = &mcp_config->routes[i];
        
        // Register POST handler for MCP requests
        httpd_uri_t* mcp_post = malloc(sizeof(httpd_uri_t));
        if (!mcp_post) {
            ESP_LOGE(TAG, "Failed to allocate memory for MCP POST handler");
            return ESP_ERR_NO_MEM;
        }
        
        mcp_post->uri = strdup(route->route);
        if (!mcp_post->uri) {
            ESP_LOGE(TAG, "Failed to allocate memory for route URI");
            free(mcp_post);
            return ESP_ERR_NO_MEM;
        }
        
        mcp_post->method = HTTP_POST;
        mcp_post->handler = mcp_handler;
        mcp_post->user_ctx = NULL;
        
        esp_err_t ret = httpd_register_uri_handler(server, mcp_post);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register MCP POST handler for %s: %s", 
                     route->route, esp_err_to_name(ret));
            free((void*)mcp_post->uri);
            free(mcp_post);
            return ret;
        }
        
        // Register OPTIONS handler for CORS preflight
        httpd_uri_t* mcp_options = malloc(sizeof(httpd_uri_t));
        if (!mcp_options) {
            ESP_LOGE(TAG, "Failed to allocate memory for MCP OPTIONS handler");
            return ESP_ERR_NO_MEM;
        }
        
        mcp_options->uri = strdup(route->route);
        if (!mcp_options->uri) {
            ESP_LOGE(TAG, "Failed to allocate memory for OPTIONS route URI");
            free(mcp_options);
            return ESP_ERR_NO_MEM;
        }
        
        mcp_options->method = HTTP_OPTIONS;
        mcp_options->handler = mcp_http_cors_options_handler;
        mcp_options->user_ctx = NULL;
        
        ret = httpd_register_uri_handler(server, mcp_options);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register MCP OPTIONS handler for %s: %s", 
                     route->route, esp_err_to_name(ret));
            free((void*)mcp_options->uri);
            free(mcp_options);
            return ret;
        }
        
        ESP_LOGI(TAG, "Registered MCP endpoint: %s -> %s", 
                 route->route, route->wasm_file);
    }
    
    ESP_LOGI(TAG, "All MCP endpoints registered successfully");
    return ESP_OK;
}

esp_err_t mcp_http_register_handlers(httpd_handle_t server,
                                     const mcp_http_handler_t* handlers,
                                     size_t handler_count) {
    if (!server) {
        ESP_LOGE(TAG, "Server handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!handlers) {
        ESP_LOGE(TAG, "Handlers array is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Registering %d custom HTTP handlers...", handler_count);
    
    for (size_t i = 0; i < handler_count; i++) {
        const mcp_http_handler_t* handler_config = &handlers[i];
        
        httpd_uri_t uri_config = {
            .uri = handler_config->uri,
            .method = handler_config->method,
            .handler = handler_config->handler,
            .user_ctx = handler_config->user_ctx
        };
        
        esp_err_t ret = httpd_register_uri_handler(server, &uri_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register handler for %s: %s", 
                     handler_config->uri, esp_err_to_name(ret));
            return ret;
        }
        
        ESP_LOGI(TAG, "Registered handler: %s", handler_config->uri);
    }
    
    ESP_LOGI(TAG, "All custom handlers registered successfully");
    return ESP_OK;
}

esp_err_t mcp_http_cors_options_handler(httpd_req_t *req) {
    esp_err_t ret = mcp_http_set_cors_headers(req);
    if (ret != ESP_OK) {
        return ret;
    }
    
    return httpd_resp_send(req, "", 0);
}

esp_err_t mcp_http_health_handler(httpd_req_t *req) {
    const char* health_response = "{\"status\":\"healthy\",\"service\":\"mcp-framework\"}";
    
    httpd_resp_set_type(req, "application/json");
    
    esp_err_t ret = mcp_http_set_cors_headers(req);
    if (ret != ESP_OK) {
        return ret;
    }
    
    return httpd_resp_send(req, health_response, strlen(health_response));
}

esp_err_t mcp_http_set_cors_headers(httpd_req_t *req) {
    esp_err_t ret;
    
    ret = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set CORS origin header");
        return ret;
    }
    
    ret = httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set CORS methods header");
        return ret;
    }
    
    ret = httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set CORS headers header");
        return ret;
    }
    
    return ESP_OK;
}