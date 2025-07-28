#![no_std]
#![no_main]

use core::panic::PanicInfo;

// Import host functions from the "env" module
#[link(wasm_import_module = "env")]
extern "C" {
    fn led_control(state: i32) -> i32;
    fn get_tick() -> i32;
}

// Simple test function that adds two numbers
#[no_mangle]
pub extern "C" fn add(a: i32, b: i32) -> i32 {
    a + b
}

// Simple LED blink function - just toggles based on time
#[no_mangle]
pub extern "C" fn led_control_task() -> i32 {
    unsafe {
        let tick = get_tick();
        // Toggle LED based on whether tick/1000 is even or odd
        let state = (tick / 1000) & 1;
        led_control(state);
    }
    0
}

// Panic handler required for no_std
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}