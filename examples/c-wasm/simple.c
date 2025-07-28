// Simple C WASM test functions for ESP32 WAMR testing
// This helps isolate whether the issue is with Rust WASM or WAMR configuration

// Simple add function to test basic WASM execution
int add(int a, int b) {
    return a + b;
}

// LED control function that matches our use case
int led_control(int state) {
    // This will call the native led_control function
    // In WASM, we can't directly control GPIO, so we call natives
    return state ? 1 : 0;
}

// Simple function with no parameters (like led_control_task)
int simple_task() {
    return 42;
}

// Export these functions so they can be called from the host
// The WASM compiler should make these available to WAMR