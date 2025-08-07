#include "mcp_sdk.h"
#include <stdio.h>

// Music Player MCP Module
// This demonstrates how to implement a music player using PWM for speaker control

// Configuration
#define DEFAULT_PWM_PIN 25
#define DEFAULT_VOLUME 50  // 0-100%
#define MAX_NOTE_LENGTH 8  // "C#7:1000"
#define MAX_MELODY_LENGTH 512

// Note frequency table (A4 = 440Hz reference)
typedef struct {
    const char* note_name;
    float frequency;
} note_freq_t;

// Chromatic scale from C4 to B7 (48 notes)
static const note_freq_t note_frequencies[] = {
    // C4 octave (Middle C octave)
    {"C4",  261.63f}, {"C#4", 277.18f}, {"Db4", 277.18f}, {"D4",  293.66f},
    {"D#4", 311.13f}, {"Eb4", 311.13f}, {"E4",  329.63f}, {"F4",  349.23f},
    {"F#4", 369.99f}, {"Gb4", 369.99f}, {"G4",  392.00f}, {"G#4", 415.30f},
    {"Ab4", 415.30f}, {"A4",  440.00f}, {"A#4", 466.16f}, {"Bb4", 466.16f},
    {"B4",  493.88f},
    
    // C5 octave
    {"C5",  523.25f}, {"C#5", 554.37f}, {"Db5", 554.37f}, {"D5",  587.33f},
    {"D#5", 622.25f}, {"Eb5", 622.25f}, {"E5",  659.25f}, {"F5",  698.46f},
    {"F#5", 739.99f}, {"Gb5", 739.99f}, {"G5",  783.99f}, {"G#5", 830.61f},
    {"Ab5", 830.61f}, {"A5",  880.00f}, {"A#5", 932.33f}, {"Bb5", 932.33f},
    {"B5",  987.77f},
    
    // C6 octave
    {"C6",  1046.50f}, {"C#6", 1108.73f}, {"Db6", 1108.73f}, {"D6",  1174.66f},
    {"D#6", 1244.51f}, {"Eb6", 1244.51f}, {"E6",  1318.51f}, {"F6",  1396.91f},
    {"F#6", 1479.98f}, {"Gb6", 1479.98f}, {"G6",  1567.98f}, {"G#6", 1661.22f},
    {"Ab6", 1661.22f}, {"A6",  1760.00f}, {"A#6", 1864.66f}, {"Bb6", 1864.66f},
    {"B6",  1975.53f},
    
    // C7 octave
    {"C7",  2093.00f}, {"C#7", 2217.46f}, {"Db7", 2217.46f}, {"D7",  2349.32f},
    {"D#7", 2489.02f}, {"Eb7", 2489.02f}, {"E7",  2637.02f}, {"F7",  2793.83f},
    {"F#7", 2959.96f}, {"Gb7", 2959.96f}, {"G7",  3135.96f}, {"G#7", 3322.44f},
    {"Ab7", 3322.44f}, {"A7",  3520.00f}, {"A#7", 3729.31f}, {"Bb7", 3729.31f},
    {"B7",  3951.07f}
};

static const int note_count = sizeof(note_frequencies) / sizeof(note_freq_t);

// Global state
static int current_volume = DEFAULT_VOLUME;
static int current_pin = DEFAULT_PWM_PIN;
static bool is_playing = false;

// Forward declarations
static float get_note_frequency(const char* note);
static int parse_note_duration(const char* note_with_duration, char* note_out, int* duration_out);
static mcp_result_t play_single_note(const char* note, int duration, int volume);

// Tool: play_note - Play a single note with optional duration and volume
mcp_result_t play_note_handler(const char* params) {
    char note[MAX_NOTE_LENGTH] = "C4";
    int duration = 1000;  // Default 1 second
    int volume = current_volume;
    char result_json[256];
    
    log_info("MUSIC", "play_note_handler called");
    
    // Parse parameters
    if (mcp_parse_string_param(params, "note", note, sizeof(note)) != 0) {
        return mcp_error(-1, "Missing 'note' parameter");
    }
    
    // Optional duration
    mcp_parse_int_param(params, "duration", &duration);
    
    // Optional volume
    mcp_parse_int_param(params, "volume", &volume);
    
    // Validate parameters
    if (duration < 1 || duration > 10000) {
        return mcp_error(-2, "Duration must be between 1 and 10000 ms");
    }
    
    if (volume < 0 || volume > 100) {
        return mcp_error(-3, "Volume must be between 0 and 100");
    }
    
    // Play the note
    mcp_result_t result = play_single_note(note, duration, volume);
    if (result.code != 0) {
        return result;
    }
    
    // Create success response
    snprintf(result_json, sizeof(result_json),
        "{\"note\":\"%s\",\"duration\":%d,\"volume\":%d,\"pin\":%d}",
        note, duration, volume, current_pin);
    
    return mcp_success(result_json);
}

// Tool: play_melody - Play a sequence of notes
mcp_result_t play_melody_handler(const char* params) {
    char melody[MAX_MELODY_LENGTH];
    int volume = current_volume;
    char result_json[256];
    
    log_info("MUSIC", "play_melody_handler called");
    
    // Parse parameters
    if (mcp_parse_string_param(params, "melody", melody, sizeof(melody)) != 0) {
        return mcp_error(-1, "Missing 'melody' parameter");
    }
    
    // Optional volume
    mcp_parse_int_param(params, "volume", &volume);
    
    if (volume < 0 || volume > 100) {
        return mcp_error(-3, "Volume must be between 0 and 100");
    }
    
    // TODO: Implement melody parsing and playback
    // For now, just return success with the melody string
    snprintf(result_json, sizeof(result_json),
        "{\"melody\":\"%s\",\"volume\":%d,\"status\":\"not_implemented\"}",
        melody, volume);
    
    return mcp_success(result_json);
}

// Tool: stop_music - Stop current playback
mcp_result_t stop_music_handler(const char* params) {
    char result_json[128];
    
    log_info("MUSIC", "stop_music_handler called");
    
    // Stop PWM if playing
    if (is_playing) {
        pwm_stop(current_pin);
        is_playing = false;
    }
    
    snprintf(result_json, sizeof(result_json),
        "{\"status\":\"stopped\",\"pin\":%d}", current_pin);
    
    return mcp_success(result_json);
}

// Tool: set_volume - Adjust playback volume
mcp_result_t set_volume_handler(const char* params) {
    int volume;
    char result_json[128];
    
    log_info("MUSIC", "set_volume_handler called");
    
    // Parse volume parameter
    if (mcp_parse_int_param(params, "volume", &volume) != 0) {
        return mcp_error(-1, "Missing 'volume' parameter");
    }
    
    if (volume < 0 || volume > 100) {
        return mcp_error(-2, "Volume must be between 0 and 100");
    }
    
    current_volume = volume;
    
    snprintf(result_json, sizeof(result_json),
        "{\"volume\":%d}", current_volume);
    
    return mcp_success(result_json);
}

// Helper: Get frequency for a note name
static float get_note_frequency(const char* note) {
    if (!note) return 0.0f;
    
    for (int i = 0; i < note_count; i++) {
        if (wasm_strcmp(note, note_frequencies[i].note_name) == 0) {
            return note_frequencies[i].frequency;
        }
    }
    
    return 0.0f; // Note not found
}

// Helper: Parse note with optional duration "C4:500" -> note="C4", duration=500
static int parse_note_duration(const char* note_with_duration, char* note_out, int* duration_out) {
    if (!note_with_duration || !note_out || !duration_out) return -1;
    
    const char* colon = strstr(note_with_duration, ":");
    if (!colon) {
        // No duration specified, use default
        strcpy(note_out, note_with_duration);
        *duration_out = 500; // Default 500ms
        return 0;
    }
    
    // Copy note part
    int note_len = colon - note_with_duration;
    if (note_len >= MAX_NOTE_LENGTH) return -1;
    
    wasm_strncpy(note_out, note_with_duration, note_len);
    note_out[note_len] = '\0';
    
    // Parse duration
    *duration_out = atoi(colon + 1);
    
    return 0;
}

// Helper: Play a single note using PWM
static mcp_result_t play_single_note(const char* note, int duration, int volume) {
    float frequency = get_note_frequency(note);
    
    if (frequency <= 0.0f) {
        return mcp_error(-4, "Unknown note name");
    }
    
    log_info("MUSIC", "Playing note with PWM");
    
    // Initialize PWM for the frequency
    if (pwm_init(current_pin, (int32_t)frequency) != 0) {
        return mcp_error(-5, "Failed to initialize PWM");
    }
    
    // Set duty cycle based on volume (0-100% -> 0-50% duty for speaker)
    int duty = (volume * 50) / 100;  // Scale to 0-50% duty cycle
    if (pwm_set_duty(current_pin, duty) != 0) {
        return mcp_error(-6, "Failed to set PWM duty cycle");
    }
    
    is_playing = true;
    
    // TODO: Implement non-blocking delay for duration
    // For now, the host application will need to handle timing
    
    return mcp_success("{}");
}

// MCP Tool registration
static mcp_tool_t music_tools[] = {
    {
        .name = "play_note",
        .description = "Play a single musical note with optional duration and volume",
        .handler = play_note_handler
    },
    {
        .name = "play_melody", 
        .description = "Play a sequence of notes as a melody",
        .handler = play_melody_handler
    },
    {
        .name = "stop_music",
        .description = "Stop current music playback",
        .handler = stop_music_handler
    },
    {
        .name = "set_volume",
        .description = "Set playback volume (0-100)",
        .handler = set_volume_handler
    }
};

// Module initialization
void _start(void) {
    log_info("MUSIC", "Music Player module starting");
    
    // Initialize MCP server
    if (mcp_server_init("music_player", "1.0.0") != 0) {
        log_error("MUSIC", "Failed to initialize MCP server");
        return;
    }
    
    // Register tools
    int tool_count = sizeof(music_tools) / sizeof(mcp_tool_t);
    for (int i = 0; i < tool_count; i++) {
        if (mcp_register_tool(&music_tools[i]) != 0) {
            log_error("MUSIC", "Failed to register tool");
            return;
        }
    }
    
    log_info("MUSIC", "Music Player module initialized successfully");
}

// Main MCP request handler (required export)
int32_t handle_mcp_request(const char* request, char* response, int32_t max_len) {
    return mcp_handle_request(request, response, max_len);
}