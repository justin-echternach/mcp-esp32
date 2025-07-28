// Simple LED control WASM module in C
// This should be compatible with WAMR on ESP32

// Import declarations for host functions
extern int led_control(int state);
extern int get_tick(void);

// Export functions for the host to call
int add(int a, int b) {
    return a + b;
}

int led_control_task(void) {
    int tick = get_tick();
    int state = (tick / 1000) & 1;  // Toggle every second
    led_control(state);
    return 0;
}