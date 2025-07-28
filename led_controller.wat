(module
  ;; Import host functions
  (import "env" "led_control" (func $led_control (param i32) (result i32)))
  (import "env" "get_tick" (func $get_tick (result i32)))
  
  ;; Memory (1 page = 64KB)
  (memory 1)
  
  ;; Export memory so host can access it
  (export "memory" (memory 0))
  
  ;; LED control function
  (func $led_control_task (export "led_control_task") (result i32)
    ;; Get current tick
    call $get_tick
    
    ;; Divide by 1000
    i32.const 1000
    i32.div_u
    
    ;; Get bit 0 (even/odd)
    i32.const 1
    i32.and
    
    ;; Call led_control with state
    call $led_control
    drop  ;; Drop the return value
    
    ;; Return 0
    i32.const 0
  )
  
  ;; Test add function
  (func $add (export "add") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add
  )
)