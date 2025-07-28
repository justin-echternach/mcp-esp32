// Minimal C WASM test - no global initialization, just simple functions
// This should avoid any post-instantiate function calls that might trigger pthread issues

// Simple add function - pure computation, no dependencies
__attribute__((export_name("add")))
int add(int a, int b) {
    return a + b;
}

// Simple function that returns a constant - no arguments, no dependencies  
__attribute__((export_name("get_value")))
int get_value() {
    return 42;
}