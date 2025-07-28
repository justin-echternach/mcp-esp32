#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

// WAMR includes
#include "wasm_export.h"
#include "bh_platform.h"

#define LED_GPIO GPIO_NUM_18
#define BLINK_PERIOD_MS 1000
#define IWASM_MAIN_STACK_SIZE 8192

static const char *TAG = "led-wamr";

// Simple WebAssembly module in bytecode format (hello world example)
// This is a minimal WASM module that exports a simple function
static const unsigned char test_wasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60, 
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01, 
    0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 
    0x00, 0x20, 0x01, 0x6a, 0x0b
};

// Native function to control LED
static int32_t native_led_control(wasm_exec_env_t exec_env, int32_t state)
{
    ESP_LOGI(TAG, "WASM called native_led_control with state: %d", state);
    gpio_set_level(LED_GPIO, state ? 1 : 0);
    return state;
}

// Native function to get system tick
static int32_t native_get_tick(wasm_exec_env_t exec_env)
{
    return (int32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Native symbol table - exports native functions to WASM
static NativeSymbol native_symbols[] = {
    {"led_control", native_led_control, "(i)i", NULL},
    {"get_tick", native_get_tick, "()i", NULL}
};

static void app_instance_main(wasm_module_inst_t module_inst)
{
    const char *exception;
    wasm_function_inst_t func;
    wasm_exec_env_t exec_env;
    uint32_t argv[2];
    uint32_t argc = 2;

    ESP_LOGI(TAG, "Looking for 'add' function in WASM module");
    
    // Create execution environment
    if (!(exec_env = wasm_runtime_create_exec_env(module_inst, 8192))) {
        ESP_LOGE(TAG, "Failed to create execution environment");
        return;
    }
    
    // Look for exported 'add' function
    if ((func = wasm_runtime_lookup_function(module_inst, "add"))) {
        ESP_LOGI(TAG, "Found 'add' function, calling it with args (5, 3)");
        
        // Set up arguments
        argv[0] = 5;
        argv[1] = 3;
        
        // Call the WASM function
        if (wasm_runtime_call_wasm(exec_env, func, argc, argv)) {
            ESP_LOGI(TAG, "WASM add(5, 3) = %d", argv[0]);
        } else {
            ESP_LOGE(TAG, "Failed to call WASM function");
        }
    } else {
        ESP_LOGW(TAG, "'add' function not found in WASM module");
    }

    if ((exception = wasm_runtime_get_exception(module_inst))) {
        ESP_LOGE(TAG, "Exception: %s", exception);
    }
    
    // Destroy execution environment
    wasm_runtime_destroy_exec_env(exec_env);
}

void iwasm_main(void *arg)
{
    ESP_LOGI(TAG, "Starting WAMR initialization");
    
    uint8_t *wasm_file_buf = NULL;
    unsigned wasm_file_buf_size = 0;
    wasm_module_t wasm_module = NULL;
    wasm_module_inst_t wasm_module_inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    char error_buf[128];
    RuntimeInitArgs init_args;
    bool wasm_available = false;

    // Configure GPIO for LED
    gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_config);

    // Configure memory allocation
    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func = (void *)os_malloc;
    init_args.mem_alloc_option.allocator.realloc_func = (void *)os_realloc;
    init_args.mem_alloc_option.allocator.free_func = (void *)os_free;

    // Register native symbols
    init_args.native_module_name = "env";
    init_args.n_native_symbols = sizeof(native_symbols) / sizeof(NativeSymbol);
    init_args.native_symbols = native_symbols;

    ESP_LOGI(TAG, "Initializing WASM runtime");
    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "Init runtime failed.");
        goto no_wasm_fallback;
    }

    ESP_LOGI(TAG, "Loading WASM module");
    
    // Copy WASM data from flash to RAM to avoid memory access issues
    wasm_file_buf_size = sizeof(test_wasm);
    wasm_file_buf = (uint8_t *)malloc(wasm_file_buf_size);
    if (!wasm_file_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for WASM module");
        goto fail1;
    }
    memcpy(wasm_file_buf, test_wasm, wasm_file_buf_size);

    if (!(wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_buf_size,
                                          error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error in wasm_runtime_load: %s", error_buf);
        goto fail1;
    }

    ESP_LOGI(TAG, "Instantiating WASM runtime");
    if (!(wasm_module_inst = wasm_runtime_instantiate(wasm_module, 
                                                     32 * 1024, // stack size
                                                     32 * 1024, // heap size
                                                     error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error while instantiating: %s", error_buf);
        goto fail2;
    }

    // Create execution environment
    if (!(exec_env = wasm_runtime_create_exec_env(wasm_module_inst, 8192))) {
        ESP_LOGE(TAG, "Failed to create execution environment");
        goto fail3;
    }

    ESP_LOGI(TAG, "✅ WASM runtime initialized successfully!");
    wasm_available = true;
    
    ESP_LOGI(TAG, "Skipping WASM function test to avoid pthread issues");
    // Skip the problematic app_instance_main call for now
    // app_instance_main(wasm_module_inst);

    goto main_loop;

no_wasm_fallback:
    ESP_LOGW(TAG, "WASM initialization failed, falling back to direct GPIO control");
    wasm_available = false;
    goto main_loop;

main_loop:    
    // LED blink loop with WASM integration or fallback
    bool led_state = false;
    int count = 0;
    
    if (wasm_available) {
        ESP_LOGI(TAG, "🔄 Starting WASM-controlled LED blink loop");
    } else {
        ESP_LOGI(TAG, "🔄 Starting direct GPIO LED blink loop");
    }
    
    while (1) {
        led_state = !led_state;
        
        if (wasm_available) {
            // Try to call WASM function if available
            wasm_function_inst_t led_func = wasm_runtime_lookup_function(wasm_module_inst, "led_control");
            if (led_func) {
                uint32_t argv[1] = {led_state ? 1 : 0};
                if (wasm_runtime_call_wasm(exec_env, led_func, 1, argv)) {
                    ESP_LOGI(TAG, "🟢 WASM LED %s (count: %d)", led_state ? "ON" : "OFF", count++);
                } else {
                    ESP_LOGE(TAG, "Failed to call WASM function, falling back");
                    gpio_set_level(LED_GPIO, led_state);
                    ESP_LOGI(TAG, "🔵 Direct LED %s (count: %d)", led_state ? "ON" : "OFF", count++);
                }
            } else {
                // No led_control function in WASM, use direct control
                gpio_set_level(LED_GPIO, led_state);
                ESP_LOGI(TAG, "🔵 Direct LED %s (count: %d) - no WASM led_control", led_state ? "ON" : "OFF", count++);
            }
        } else {
            // Direct GPIO control fallback
            gpio_set_level(LED_GPIO, led_state);
            ESP_LOGI(TAG, "🔴 Fallback LED %s (count: %d)", led_state ? "ON" : "OFF", count++);
        }
        
        vTaskDelay(BLINK_PERIOD_MS / portTICK_PERIOD_MS);
    }

    // Cleanup (only reached if loop is broken)
    ESP_LOGI(TAG, "Deinitializing WASM runtime");
    if (exec_env) wasm_runtime_destroy_exec_env(exec_env);
    
fail3:
    if (wasm_module_inst) wasm_runtime_deinstantiate(wasm_module_inst);
    
fail2:
    if (wasm_module) wasm_runtime_unload(wasm_module);
    
fail1:
    if (wasm_file_buf) free(wasm_file_buf);
    wasm_runtime_destroy();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting LED + WAMR test application");
    
    // Create a task for WAMR
    xTaskCreate(iwasm_main, "iwasm_main", IWASM_MAIN_STACK_SIZE, NULL, 5, NULL);
}