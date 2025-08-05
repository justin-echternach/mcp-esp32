#ifndef MCP_CONFIG_H
#define MCP_CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ========== Configuration Constants ==========

#define MAX_PROTECTED_PINS 16
#define MAX_MCP_ROUTES 8
#define MAX_ROUTE_LENGTH 64
#define MAX_FILENAME_LENGTH 128

// ========== Configuration Structures ==========

/**
 * @brief GPIO protection configuration
 * 
 * Defines which pins are protected from WASM access for system safety
 */
typedef struct {
    int protected_pins[MAX_PROTECTED_PINS];     // System-critical pins (bootloader, flash, etc.)
    int protected_pin_count;
    int input_only_pins[MAX_PROTECTED_PINS];    // ADC pins that cannot be outputs
    int input_only_pin_count;
    int max_current_pins[MAX_PROTECTED_PINS];   // Pins with high current risk
    int max_current_pin_count;
} gpio_protection_config_t;

/**
 * @brief MCP route configuration
 * 
 * Maps HTTP routes to WASM module files
 */
typedef struct {
    char route[MAX_ROUTE_LENGTH];               // HTTP route (e.g., "/mcp/led")
    char wasm_file[MAX_FILENAME_LENGTH];        // WASM filename (e.g., "led_ctrl.was")
} mcp_route_config_t;

/**
 * @brief System configuration parameters
 * 
 * Runtime limits and memory settings
 */
typedef struct {
    int max_wasm_modules;                       // Maximum concurrent WASM modules
    int wasm_heap_size;                         // WASM heap size in bytes
    int wasm_stack_size;                        // WASM stack size in bytes
    int sse_max_clients;                        // Maximum SSE clients
    int mcp_timeout_ms;                         // MCP request timeout in milliseconds
} system_config_t;

/**
 * @brief Complete MCP server configuration
 * 
 * Main configuration structure containing all subsystem configs
 */
typedef struct {
    gpio_protection_config_t gpio_protection;  // GPIO access control
    mcp_route_config_t routes[MAX_MCP_ROUTES]; // Route to WASM mappings
    int route_count;                            // Number of active routes
    system_config_t system;                     // System limits and settings
    bool require_auth;                          // Enable authentication (future)
    int rate_limit_per_minute;                  // Rate limiting (future)
} mcp_config_t;

#ifdef __cplusplus
}
#endif

#endif // MCP_CONFIG_H