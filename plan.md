Implementation Plan: DMA Event Queue + IRAM Functions Solution

  Overview

  Create a parallel implementation alongside the existing task-based approach, selectable via a parameter or compile-time flag.

  Phase 1: Infrastructure Setup

  1.1 Add Configuration Flag

  File: mcp_host_functions.c
  - Add enum for audio mode selection:
  typedef enum {
      I2S_MODE_TASK = 0,      // Current implementation
      I2S_MODE_DMA_EVENT = 1  // New DMA event-driven
  } i2s_audio_mode_t;
  - Add static variable to track mode
  - Modify native_i2s_init() to accept mode parameter

  1.2 Create IRAM Section Definitions

  File: mcp_host_functions.c or new mcp_audio_iram.c
  - Define IRAM_ATTR functions for critical audio generation
  - Separate sine table access functions marked for IRAM
  - Create DRAM_ATTR for data that must be in internal RAM

  Phase 2: DMA Event Queue Implementation

  2.1 Modify I2S Initialization

  Location: native_i2s_init()
  - When in DMA_EVENT mode:
    - Create I2S event queue during i2s_driver_install()
    - Increase DMA descriptors for smoother operation
    - Configure for optimal buffer sizes (power of 2)

  2.2 Create Event Queue Handler Task

  New Function: i2s_event_queue_task()
  - Higher priority than current (24-25)
  - Smaller stack (4KB)
  - Pinned to Core 1
  - Monitors I2S event queue
  - Handles I2S_EVENT_TX_DONE events

  2.3 Create IRAM Audio Generation Functions

  New Functions in IRAM:
  - IRAM_ATTR generate_sine_samples() - Core sample generation
  - IRAM_ATTR get_sine_value() - Table lookup
  - IRAM_ATTR fill_dma_buffer() - Buffer filling routine

  Phase 3: Memory Management

  3.1 Pre-allocate Buffer Pool

  Location: native_i2s_init() or first tone play
  - Allocate 4-6 DMA-capable buffers
  - Use circular buffer management
  - All buffers in DMA-capable memory (MALLOC_CAP_DMA)

  3.2 Move Critical Data to DRAM

  Data to Move:
  - Sine table → DRAM_ATTR
  - Phase accumulator → DRAM_ATTR
  - Current frequency → DRAM_ATTR
  - Buffer pointers → DRAM_ATTR

  Phase 4: Interrupt and Timer Backup

  4.1 Add Hardware Timer Backup

  New Component: ESP Timer in ISR mode
  - Create high-resolution timer
  - Period = half DMA buffer duration
  - IRAM_ATTR callback function
  - Only activates if queue blocked

  4.2 Configure Interrupt Priorities

  Location: During I2S and timer setup
  - I2S interrupt: Level 3 (highest)
  - Timer interrupt: Level 2
  - Mask lower priority interrupts during critical sections

  Phase 5: Integration Points

  5.1 Modify Play Tone Function

  Location: native_i2s_play_tone()
  if (audio_mode == I2S_MODE_DMA_EVENT) {
      // Start event queue task
      // Prime DMA buffers
      // Start I2S transmission
  } else {
      // Existing task-based approach
  }

  5.2 Modify Stop Tone Function

  Location: native_i2s_stop_tone()
  - Handle both modes
  - Properly clean up event queue task
  - Ensure DMA buffers are cleared

  Phase 6: Optimization and Testing

  6.1 Add Performance Metrics

  - Add counters for:
    - DMA underruns
    - Event queue delays
    - Buffer fill times
    - Interrupt latency

  6.2 Create Mode Comparison Function

  New Function: native_i2s_get_stats()
  - Return performance metrics
  - Allow runtime comparison

  File Structure Changes

  mcp_host_functions.c
  ├── Existing task-based implementation (keep as-is)
  ├── New DMA event mode flag/enum
  ├── Modified init/play/stop functions with mode switch
  └── Performance metrics collection

  mcp_audio_iram.c (new file - optional)
  ├── IRAM_ATTR audio generation functions
  ├── IRAM_ATTR buffer management
  ├── IRAM_ATTR sine table operations
  └── Event queue task implementation

  mcp_audio_dma.h (new file - optional)
  ├── DMA buffer structures
  ├── Event queue configuration
  └── IRAM function declarations

  Implementation Steps Order

  1. Add mode selection infrastructure (minimal change)
  2. Create IRAM functions (parallel development)
  3. Implement event queue task (new code, no conflicts)
  4. Integrate with existing play/stop (switch statement)
  5. Add performance metrics (debugging/comparison)
  6. Test both modes side-by-side
  7. Optimize buffer sizes and priorities

  Key Decision Points

  1. Single file vs. multiple files
    - Single: Easier to maintain, everything in one place
    - Multiple: Cleaner separation, easier to conditionally compile
  2. Runtime vs. compile-time selection
    - Runtime: Easy A/B testing
    - Compile-time: Smaller code size
  3. Buffer pool size
    - More buffers = smoother but more RAM
    - Fewer buffers = less RAM but risk of underrun
  4. Event queue size
    - Larger = more tolerance for delays
    - Smaller = faster response but risk of overflow

  Testing Strategy

  1. Phase 1: Ensure existing mode still works
  2. Phase 2: Test DMA event mode with simple tones
  3. Phase 3: Stress test with WiFi activity
  4. Phase 4: Compare metrics between modes
  5. Phase 5: Long duration stability test

  Rollback Safety

  - All changes are additive or behind mode flag
  - Existing implementation untouched
  - Can disable new mode via single flag
  - Easy to remove if not improving performance

  This plan allows incremental implementation while maintaining the working system, with clear comparison between approaches.