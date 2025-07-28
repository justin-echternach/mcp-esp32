// Simple LED blinker for WASM
// Compile with: /opt/wasi-sdk/bin/clang -O3 -nostdlib -z stack-size=1024 -Wl,--initial-memory=65536 -Wl,--export-all -Wl,--no-entry -Wl,--strip-all -Wl,--allow-undefined -o led_blink.wasm led_blink.c

// Import host functions
__attribute__((import_module("env")))
__attribute__((import_name("led_control")))
int led_control(int state);

__attribute__((import_module("env")))
__attribute__((import_name("get_tick")))
int get_tick(void);

// State variables
static int led_state = 0;
static int last_tick = 0;

// Main LED control function - will be called repeatedly by host
__attribute__((export_name("led_control_task")))
int led_control_task(void) {
    int current_tick = get_tick();
    
    // Toggle LED every 1000ms
    if (current_tick - last_tick >= 1000) {
        led_state = led_state ? 0 : 1;
        led_control(led_state);
        last_tick = current_tick;
    }
    
    return 0;
}

// Also export a simple add function for testing
__attribute__((export_name("add")))
int add(int a, int b) {
    return a + b;
}