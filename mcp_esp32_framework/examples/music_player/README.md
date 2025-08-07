# Music Player MCP Example

This example demonstrates how to create a music player MCP server using PWM to drive a speaker connected to ESP32 GPIO pins.

## Hardware Requirements

- ESP32 development board
- 8Ω speaker
- Speaker driver board (amplifier)
- Breadboard and connecting wires

## Hardware Connections

| Component | ESP32 Pin | Notes |
|-----------|-----------|-------|
| PWM Output | GPIO 25 | Connect to driver board signal input |
| Driver VCC | 3.3V or 5V | Depending on driver board requirements |
| Driver GND | GND | Common ground |

## Features

- **Play Single Notes**: Parse note strings like "C6", "A4", "F#5"
- **Note Durations**: Specify duration in milliseconds "C6:500"
- **Simple Melodies**: Play sequences "C6:500,D6:250,E6:250"
- **Volume Control**: PWM duty cycle adjustment
- **Stop/Start Control**: Stop current playback

## Supported Note Format

### Single Notes
- Format: `[Note][Octave]` (e.g., "C4", "A5", "F#6")
- Notes: C, C#/Db, D, D#/Eb, E, F, F#/Gb, G, G#/Ab, A, A#/Bb, B
- Octaves: 4-7 (C4 = middle C = 261.63Hz, A4 = 440Hz)

### Notes with Duration
- Format: `[Note][Octave]:[Duration]` (e.g., "C6:500")
- Duration in milliseconds

### Melodies
- Format: `[Note]:[Duration],[Note]:[Duration],...`
- Example: "C6:500,D6:250,E6:250,F6:250,G6:1000"

## MCP Tools

1. **play_note** - Play a single note
   ```json
   {"note": "C6", "duration": 500, "volume": 50}
   ```

2. **play_melody** - Play a sequence of notes
   ```json
   {"melody": "C6:500,D6:250,E6:250", "volume": 50}
   ```

3. **stop_music** - Stop current playback
   ```json
   {}
   ```

4. **set_volume** - Adjust volume (0-100)
   ```json
   {"volume": 75}
   ```

## Building

```bash
./build.sh
```

## Testing

After building and deploying the WASM file:

```bash
# Play middle C for 1 second
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"play_note","arguments":{"note":"C4","duration":1000,"volume":50}},"id":1}'

# Play a simple melody
curl -X POST http://esp32-ip/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"play_melody","arguments":{"melody":"C4:500,E4:500,G4:500,C5:1000","volume":60}},"id":2}'
```

## Technical Details

- Uses ESP32 LEDC (LED Control) peripheral for PWM generation
- Frequency range: ~261Hz (C4) to ~2093Hz (C7)
- PWM resolution: 12-bit for fine frequency control
- Timer: 1MHz base frequency for accurate note frequencies