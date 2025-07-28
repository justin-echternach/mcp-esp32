// Simple LED controller for WASM

// Import host functions
__attribute__((import_module("env"))) __attribute__((import_name("led_control")))
int led_control(int state);

__attribute__((import_module("env"))) __attribute__((import_name("get_tick")))
int get_tick(void);

// Export functions
__attribute__((export_name("led_control_task")))
int led_control_task(void) {
    int tick = get_tick();
    int state = (tick / 1000) & 1;
    led_control(state);
    return 0;
}

__attribute__((export_name("add")))
int add(int a, int b) {
    return a + b;
}