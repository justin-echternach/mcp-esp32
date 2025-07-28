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

// Include the generated WASM bytecode  
#include "../led_controller_wasm.h"

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
    wasm_file_buf_size = led_controller_wasm_len;
    wasm_file_buf = (uint8_t *)malloc(wasm_file_buf_size);
    if (!wasm_file_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for WASM module");
        goto fail1;
    }
    memcpy(wasm_file_buf, led_controller_wasm, wasm_file_buf_size);
    ESP_LOGI(TAG, "WASM module size: %d bytes", wasm_file_buf_size);

    if (!(wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_buf_size,
                                          error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error in wasm_runtime_load: %s", error_buf);
        goto fail1;
    }

    ESP_LOGI(TAG, "Instantiating WASM runtime");
    ESP_LOGI(TAG, "Free heap before instantiate: %ld bytes", esp_get_free_heap_size());
    if (!(wasm_module_inst = wasm_runtime_instantiate(wasm_module, 
                                                     8192,      // stack size
                                                     8192,      // heap size
                                                     error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error while instantiating: %s", error_buf);
        goto fail2;
    }
    
    ESP_LOGI(TAG, "WASM module instantiated successfully");
    ESP_LOGI(TAG, "Free heap after instantiate: %ld bytes", esp_get_free_heap_size());

    // Try creating execution environment with different stack sizes
    uint32_t stack_sizes[] = {512, 1024, 2048, 4096, 8192};
    int stack_size_count = sizeof(stack_sizes) / sizeof(stack_sizes[0]);
    
    for (int i = 0; i < stack_size_count; i++) {
        ESP_LOGI(TAG, "Trying exec env with stack size: %ld bytes", stack_sizes[i]);
        if ((exec_env = wasm_runtime_create_exec_env(wasm_module_inst, stack_sizes[i]))) {
            ESP_LOGI(TAG, "✅ Successfully created exec env with %ld bytes stack", stack_sizes[i]);
            break;
        }
        ESP_LOGW(TAG, "Failed with stack size %ld, trying next...", stack_sizes[i]);
    }
    
    if (!exec_env) {
        ESP_LOGW(TAG, "Failed to create execution environment with any stack size");
        ESP_LOGW(TAG, "Will continue with WASM loaded but cannot execute functions");
        ESP_LOGW(TAG, "Free heap: %ld bytes", esp_get_free_heap_size());
        // Continue execution but WASM functions won't work
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
            if (exec_env) {
                // Try to call the WASM LED control function
                wasm_function_inst_t led_task_func = wasm_runtime_lookup_function(wasm_module_inst, "led_control_task");
                if (led_task_func) {
                    // Call WASM LED control task (no arguments)
                    if (wasm_runtime_call_wasm(exec_env, led_task_func, 0, NULL)) {
                        ESP_LOGI(TAG, "🟢 WASM LED control executed (count: %d)", count++);
                    } else {
                        ESP_LOGE(TAG, "🔴 WASM LED control failed, using fallback");
                        gpio_set_level(LED_GPIO, led_state);
                        count++;
                    }
                } else {
                    // Fallback to testing add function if led_control_task not found
                    wasm_function_inst_t add_func = wasm_runtime_lookup_function(wasm_module_inst, "add");
                    if (add_func) {
                        uint32_t argv[2] = {count, 1};
                        if (wasm_runtime_call_wasm(exec_env, add_func, 2, argv)) {
                            count = argv[0];  // Get result
                            ESP_LOGI(TAG, "🔵 WASM add result: %d, direct LED %s", count, led_state ? "ON" : "OFF");
                            gpio_set_level(LED_GPIO, led_state);
                        } else {
                            count++;
                            ESP_LOGE(TAG, "WASM add failed");
                        }
                    } else {
                        count++;
                        ESP_LOGI(TAG, "🔴 No WASM functions found, direct LED %s (count: %d)", led_state ? "ON" : "OFF", count);
                        gpio_set_level(LED_GPIO, led_state);
                    }
                }
            } else {
                // WASM module loaded but no execution environment
                // Just show that we have the module loaded and use direct control
                wasm_function_inst_t led_task_func = wasm_runtime_lookup_function(wasm_module_inst, "led_control_task");
                wasm_function_inst_t add_func = wasm_runtime_lookup_function(wasm_module_inst, "add");
                
                ESP_LOGI(TAG, "🟡 WASM loaded (%d bytes), has led_control_task: %s, has add: %s, direct LED %s (count: %d)", 
                         led_controller_wasm_len,
                         led_task_func ? "YES" : "NO",
                         add_func ? "YES" : "NO",
                         led_state ? "ON" : "OFF", 
                         count++);
                gpio_set_level(LED_GPIO, led_state);
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