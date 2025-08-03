# ESP32 MCP WASM Kickass Demo Plan

## Overview
Create an impressive demo showcasing the ESP32 MCP server with LLM control of:
1. **8 RGB LED Module** - Addressable WS2812 LEDs with animations
2. **Speaker/Buzzer** - Play melodies via simple text-based music notation

## Hardware Setup

### Components
- **ESP32-WROVER-E** (with 4MB PSRAM enabled)
- **Freenove 8 RGB LED Module** (WS2812-compatible)
  - Single data pin control for 8 individually addressable RGB LEDs
  - Connections: S (Signal), V (5V), G (Ground)
- **Speaker/Buzzer**
  - Connect through 100-220Ω resistor for protection
  - Use PWM-capable GPIO

### GPIO Allocation
**Already Used:**
- GPIO 5: SD Card CS (SPI mode) / Single LED (current demo)
- GPIO 18: SD Card CLK (SPI)
- GPIO 19: SD Card MISO (SPI)
- GPIO 23: SD Card MOSI (SPI)
- GPIO 14, 15, 2: SD Card (SDMMC fallback)

**Available for Demo:**
- GPIO 26: RGB LED Module Data Pin (WS2812)
- GPIO 25: Speaker PWM Output
- GPIO 4, 12, 13, 16, 17, 21, 22, 27, 32, 33: Reserved for future expansion

## Architecture

### WASM Modules
1. **`rgb_led_controller.was`** - 8 RGB LED control
2. **`music_player.was`** - Melody playback
3. **`demo_orchestrator.was`** - Combined demo with synchronized LEDs + music

### MCP Tools Design

#### RGB LED Controller Tools

```json
{
  "tools": [
    {
      "name": "set_led_color",
      "description": "Set a single LED to specific RGB color",
      "parameters": {
        "led": "LED number (1-8)",
        "red": "Red value (0-255)",
        "green": "Green value (0-255)",
        "blue": "Blue value (0-255)"
      }
    },
    {
      "name": "set_all_leds",
      "description": "Set all LEDs to same color",
      "parameters": {
        "red": "Red value (0-255)",
        "green": "Green value (0-255)",
        "blue": "Blue value (0-255)"
      }
    },
    {
      "name": "run_pattern",
      "description": "Run predefined pattern",
      "parameters": {
        "pattern": "Pattern name: rainbow, chase, breathe, fire, police, loading",
        "speed": "Animation speed (1-10, default 5)"
      }
    },
    {
      "name": "create_custom_animation",
      "description": "Create custom LED sequence",
      "parameters": {
        "frames": "Array of 8 RGB values per frame",
        "frame_delay": "Delay between frames in ms"
      }
    },
    {
      "name": "stop_animation",
      "description": "Stop current animation"
    }
  ]
}
```

#### Music Player Tools

```json
{
  "tools": [
    {
      "name": "play_melody",
      "description": "Play a melody using musical notation",
      "parameters": {
        "melody": "Array of note objects with 'note' and 'duration'",
        "tempo": "Optional BPM (default 120)"
      }
    },
    {
      "name": "play_tone",
      "description": "Play a single tone",
      "parameters": {
        "frequency": "Frequency in Hz (or note like 'C4')",
        "duration": "Duration in milliseconds"
      }
    },
    {
      "name": "play_rtttl",
      "description": "Play RTTTL format melody",
      "parameters": {
        "rtttl": "RTTTL string like 'Song:d=4,o=5,b=120:c,d,e,f'"
      }
    },
    {
      "name": "stop_music",
      "description": "Stop current playback"
    }
  ]
}
```

## Music Format Design

### Primary Format: JSON Note Array
Simple, LLM-friendly format that aligns with existing JSON-RPC infrastructure:

```json
{
  "melody": [
    {"note": "C4", "duration": 500},
    {"note": "E4", "duration": 250},
    {"note": "G4", "duration": 250},
    {"note": "C5", "duration": 1000},
    {"note": "REST", "duration": 500}
  ]
}
```

### Note Naming Convention
- Standard notation: `C4`, `D#4`, `Gb5`
- Octave range: 0-8 (focus on 3-6 for speaker)
- Special: `REST` or duration=0 for silence
- Support both sharps (#) and flats (b)

### Alternative Format: RTTTL (Ring Tone Text Transfer Language)
Compact format for predefined songs:
```
"TwinkleTwinkle:d=4,o=5,b=120:c,c,g,g,a,a,2g,f,f,e,e,d,d,2c"
```

## Implementation Details

### Host Functions (ESP32 Side)

```c
// RGB LED Control (using RMT driver for WS2812)
static int32_t native_ws2812_set_pixel(wasm_exec_env_t exec_env, 
    int32_t pixel, int32_t r, int32_t g, int32_t b);
static int32_t native_ws2812_show(wasm_exec_env_t exec_env);
static int32_t native_ws2812_clear(wasm_exec_env_t exec_env);

// Music Playback (using LEDC PWM)
static int32_t native_play_frequency(wasm_exec_env_t exec_env, 
    int32_t freq, int32_t duration_ms);
static int32_t native_stop_tone(wasm_exec_env_t exec_env);
```

### WASM Module Structure

```c
// rgb_led_controller.c
typedef struct {
    uint8_t r, g, b;
} rgb_color_t;

static rgb_color_t led_buffer[8];
static bool animation_running = false;

// Pattern implementations
void pattern_rainbow(int speed);
void pattern_chase(rgb_color_t color, int speed);
void pattern_breathe(rgb_color_t color, int speed);
void pattern_fire(int speed);
void pattern_police(int speed);

// MCP tool handlers
mcp_result_t handle_set_led_color(const char* params);
mcp_result_t handle_run_pattern(const char* params);
```

## Demo Scenarios

### 1. "Knight Rider" Demo
```
LLM: "Create a red chase light effect like Knight Rider"
→ Calls: run_pattern({"pattern": "chase", "color": "red", "speed": 8})
```

### 2. "Happy Birthday" Celebration
```
LLM: "Play Happy Birthday with rainbow LEDs"
→ Calls: play_melody() + run_pattern({"pattern": "rainbow"})
```

### 3. "System Status" Indicator
```
LLM: "Show system thinking with pulsing blue, then success with green"
→ Calls: Multiple coordinated LED and tone commands
```

### 4. "Music Visualizer"
```
LLM: "Play a melody and sync LEDs to the beat"
→ Orchestrator module coordinates music + LED timing
```

## LLM Interaction Examples

### Simple Commands
- "Turn LED 3 purple"
- "Make all LEDs breathe in blue"
- "Play middle C for 1 second"
- "Create a police siren effect"

### Complex Commands
- "Create a loading animation in green while playing ascending tones"
- "Show a fire effect on the LEDs"
- "Play Twinkle Twinkle Little Star with twinkling white LEDs"
- "Create a disco effect with random colors and beats"

## Technical Considerations

### Memory Management
- RGB LED buffer: 8 × 3 bytes = 24 bytes
- Pattern state: ~100 bytes
- Music queue: ~1KB for 50-note melody
- Total WASM module: <64KB each

### Timing Coordination
- Use FreeRTOS tasks for pattern animations
- Non-blocking music playback
- Synchronization via shared state

### Error Handling
- Invalid LED numbers (must be 1-8)
- Invalid RGB values (clamp to 0-255)
- Invalid notes (treat as REST)
- Frequency limits (20Hz - 8kHz)

## Build Process

### Compile WASM Modules
```bash
# RGB LED Controller
$WASI_SDK_PATH/bin/clang \
  --target=wasm32-wasi \
  -nostdlib \
  -O3 \
  -z stack-size=4096 \
  -Wl,--initial-memory=65536 \
  -Wl,--export=handle_mcp_request \
  -o rgb_led_controller.was \
  rgb_led_controller.c mcp_sdk.c

# Music Player
$WASI_SDK_PATH/bin/clang \
  --target=wasm32-wasi \
  -nostdlib \
  -O3 \
  -z stack-size=4096 \
  -Wl,--initial-memory=65536 \
  -Wl,--export=handle_mcp_request \
  -o music_player.was \
  music_player.c mcp_sdk.c
```

### SD Card Structure
```
/sdcard/
  /wasm/
    rgb_led_controller.was
    music_player.was
    demo_orchestrator.was
  /config/
    mcp_routes.json
```

### Route Configuration
```json
{
  "routes": [
    {
      "path": "/mcp/leds",
      "wasm": "rgb_led_controller.was"
    },
    {
      "path": "/mcp/music", 
      "wasm": "music_player.was"
    },
    {
      "path": "/mcp/demo",
      "wasm": "demo_orchestrator.was"
    }
  ]
}
```

## Testing Plan

### Phase 1: Individual Components
1. Test each LED individually with colors
2. Test speaker with single tones
3. Verify WASM module loading

### Phase 2: Patterns & Melodies
1. Test each LED pattern
2. Play simple melodies
3. Verify timing accuracy

### Phase 3: Integration
1. Synchronized LED + music
2. LLM command parsing
3. Error handling

### Phase 4: Demo Recording
1. Film impressive sequences
2. Show LLM interaction
3. Demonstrate modularity

## Future Enhancements

### Hardware
- Add more LED modules (daisy-chain)
- Multiple speakers for harmony
- Sensors for interactive demos
- Display for lyrics/status

### Software
- MIDI file support
- Custom pattern designer
- Beat detection
- Multi-track music
- WebSocket live control

## Success Metrics
- ✅ LLM can control individual LEDs
- ✅ Multiple animation patterns work smoothly
- ✅ Music playback is recognizable
- ✅ Synchronized LED + music effects
- ✅ <100ms response time
- ✅ Stable for 10+ minute demos
- ✅ Easy to add new patterns/songs

---

*This demo showcases the power of MCP + WASM on embedded systems, enabling LLMs to orchestrate complex hardware interactions through simple, modular interfaces.*