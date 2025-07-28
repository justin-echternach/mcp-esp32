// LED control WASM module
// This module directly controls the LED by calling native functions

// Import native functions from ESP32
extern int led_control(int state);
extern int get_tick(void);

// WASM function to blink LED
__attribute__((export_name("led_blink_task")))
int led_blink_task() {
    // Get current tick count to determine LED state
    int tick = get_tick();
    
    // Blink every 1000ms (approximate)
    int led_state = (tick / 1000) % 2;
    
    // Control LED through native function
    led_control(led_state);
    
    return led_state;
}

// WASM function for LED on
__attribute__((export_name("led_on")))
void led_on() {
    led_control(1);
}

// WASM function for LED off  
__attribute__((export_name("led_off")))
void led_off() {
    led_control(0);
}

// WASM function for LED toggle with counter
__attribute__((export_name("led_toggle")))
int led_toggle(int counter) {
    int state = counter % 2;
    led_control(state);
    return state;
}