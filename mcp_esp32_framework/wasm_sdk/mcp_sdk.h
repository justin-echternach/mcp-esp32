#ifndef MCP_SDK_H
#define MCP_SDK_H

#include <stdint.h>
#include <stdbool.h>

// MCP SDK for ESP32 WASM Modules
// This header provides the interface for implementing MCP servers in WebAssembly

// Maximum buffer sizes
#define MCP_MAX_TOOL_NAME 64
#define MCP_MAX_DESCRIPTION 256
#define MCP_MAX_PARAMS 2048
#define MCP_MAX_RESPONSE 1024
#define MCP_MAX_INPUT_SCHEMA 512

// Configurable dynamic buffer sizes - can be overridden at compile time
#ifndef MCP_PARAMS_BUFFER_SIZE
#define MCP_PARAMS_BUFFER_SIZE 2048
#endif

#ifndef MCP_TOOL_PARAMS_BUFFER_SIZE  
#define MCP_TOOL_PARAMS_BUFFER_SIZE 2048
#endif

#ifndef MCP_RESPONSE_BUFFER_SIZE
#define MCP_RESPONSE_BUFFER_SIZE 1024
#endif

// MCP Result structure
typedef struct {
    int32_t code;      // 0 = success, negative = error
    char message[256]; // Error message or success description
    char data[512];    // JSON data payload
} mcp_result_t;

// MCP Tool handler function signature
typedef mcp_result_t (*mcp_tool_handler_t)(const char* params);

// MCP Tool definition
typedef struct {
    char name[MCP_MAX_TOOL_NAME];
    char description[MCP_MAX_DESCRIPTION];
    char input_schema[MCP_MAX_INPUT_SCHEMA];
    mcp_tool_handler_t handler;
} mcp_tool_t;

// === Host Function Imports ===
// These functions are provided by the ESP32 host

// Host-provided formatting function
extern int snprintf(char* buffer, unsigned long size, const char* format, ...);

// Host-provided memory allocation functions
extern void* malloc(unsigned long size);
extern void free(void* ptr);

// Host-provided string functions (don't use standard library)
// Use standard signatures to avoid compiler warnings

extern unsigned long strlen(const char* s);
//extern int wasm_strcmp(const char* s1, const char* s2);
extern int strcmp(const char* s1, const char* s2);
extern char* strcpy(char* dest, const char* src);
extern char* strcat(char* dest, const char* src);
//extern char* wasm_strncpy(char* dest, const char* src, unsigned long n);
extern char* strncpy(char* dest, const char* src, unsigned long n);

extern char* strstr(const char* haystack, const char* needle);
extern int strncmp(const char* s1, const char* s2, unsigned long n);

// Host-provided conversion function
extern int atoi(const char* str);

// Logging
extern int32_t log_info(const char* tag, const char* message);
//extern int32_t log_info2(uint32_t str_offset);
//extern int32_t log_info3(char *str, uint32_t len);
extern int32_t log_error(const char* tag, const char* message);

// System Info
extern int32_t get_free_heap(void);
extern int32_t get_uptime_ms(void);
extern int32_t get_tick_count(void);

// GPIO Control
extern int32_t gpio_set_level(int32_t pin, int32_t level);
extern int32_t gpio_get_level(int32_t pin);
extern int32_t gpio_set_direction(int32_t pin, int32_t direction);
extern int32_t gpio_is_pin_protected(int32_t pin);

// PWM Control (for music/speaker applications)
extern int32_t pwm_init(int32_t pin, int32_t frequency);
extern int32_t pwm_set_duty(int32_t pin, int32_t duty_percent);
extern int32_t pwm_stop(int32_t pin);
extern int32_t pwm_set_frequency(int32_t pin, int32_t frequency);

// I2S Audio Control (high-quality audio output)
extern int32_t i2s_init(int32_t sample_rate, int32_t bits_per_sample);
extern int32_t i2s_deinit(void);
extern int32_t i2s_start(void);  // Start I2S clock generation
extern int32_t i2s_stop(void);   // Stop I2S clock generation
extern int32_t i2s_play_tone(int32_t frequency_hz, int32_t duration_ms, int32_t volume_percent);
extern int32_t i2s_stop_tone(void);
extern int32_t i2s_set_volume(int32_t volume_percent);
extern int32_t i2s_is_playing(void);
extern int32_t i2s_set_envelope(int32_t attack_ms, int32_t release_ms, int32_t envelope_type);
extern int32_t i2s_play_melody(const char* melody_str, int32_t volume_percent);
extern int32_t i2s_stop_melody(void);

// Bluetooth Audio Control (A2DP sink for wireless speakers/headphones)
extern int32_t bluetooth_init(void);
extern int32_t bluetooth_deinit(void);
extern int32_t bluetooth_scan_start(int32_t timeout_sec);
extern int32_t bluetooth_scan_get_results(char* buffer, int32_t max_len);
extern int32_t bluetooth_pair_device(const char* device_name);
extern int32_t bluetooth_connect_device(const char* device_address);
extern int32_t bluetooth_disconnect(void);
extern int32_t bluetooth_is_connected(void);
extern int32_t bluetooth_get_connected_device_name(char* buffer, int32_t max_len);
extern int32_t bluetooth_get_connected_device_address(char* buffer, int32_t max_len);
extern int32_t bluetooth_play_tone(int32_t frequency_hz, int32_t duration_ms, int32_t volume_percent);
extern int32_t bluetooth_play_melody(const char* melody_str, int32_t volume_percent);
extern int32_t bluetooth_set_volume(int32_t volume_percent);

// SSE Events
extern int32_t sse_send_event(const char* event_type, const char* data);
extern int32_t sse_send_tool_result(const char* tool_name, const char* result);



// JSON Helper Functions
extern int32_t json_parse_request(const char* json, char* method, char* params, int32_t max_len);
extern int32_t json_create_response(const char* result, char* response, int32_t max_len);
extern int32_t json_create_error(int32_t code, const char* message, char* response, int32_t max_len);



// HTTP Header Management (using buffer+length pattern)
extern int32_t set_response_header(const char* name, const char* value);
extern int32_t get_request_header(const char* name, char* buffer, int32_t buffer_size);
extern int32_t remove_response_header(const char* name);
extern int32_t list_request_headers(char* buffer, int32_t buffer_size);



// === MCP SDK Functions ===
// These functions are provided by the SDK

// Session and header management helpers
void mcp_generate_session_id(char* buffer, int32_t size);
int32_t mcp_set_session_header(const char* session_id);

// Server initialization
int32_t mcp_server_init(const char* server_name, const char* version);

// MCP protocol state
typedef enum {
    MCP_STATE_UNINITIALIZED = 0,
    MCP_STATE_INITIALIZING = 1,
    MCP_STATE_INITIALIZED = 2
} mcp_state_t;

// Tool registration
int32_t mcp_register_tool(const mcp_tool_t* tool);

// Main MCP request handler (called by ESP32)
int32_t mcp_handle_request(const char* json_request, char* response, int32_t max_len);

// Helper functions for creating results
mcp_result_t mcp_success(const char* data);
mcp_result_t mcp_error(int32_t code, const char* message);

// JSON parsing helpers for tool parameters
int32_t mcp_parse_int_param(const char* params, const char* key, int32_t* value);
int32_t mcp_parse_string_param(const char* params, const char* key, char* value, int32_t max_len);
int32_t mcp_parse_bool_param(const char* params, const char* key, bool* value);

// JSON creation helpers for tool results
int32_t mcp_create_json_int(const char* key, int32_t value, char* json, int32_t max_len);
int32_t mcp_create_json_string(const char* key, const char* value, char* json, int32_t max_len);
int32_t mcp_create_json_bool(const char* key, bool value, char* json, int32_t max_len);

// GPIO Constants (matching ESP-IDF)
#define GPIO_MODE_INPUT    0
#define GPIO_MODE_OUTPUT   1
#define GPIO_MODE_OUTPUT_OD 2
#define GPIO_MODE_INPUT_OUTPUT 3
#define GPIO_MODE_INPUT_OUTPUT_OD 4

// === Required Exports ===
// WASM modules must implement these functions:

// Called by ESP32 to handle MCP JSON-RPC requests
// Returns 0 on success, negative on error
extern int32_t handle_mcp_request(const char* request, char* response, int32_t max_len);

// Optional: Module initialization (called once when loaded)
extern void _start(void);

#endif // MCP_SDK_H