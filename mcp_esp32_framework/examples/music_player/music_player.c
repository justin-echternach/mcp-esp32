#include "mcp_sdk.h"
#include <stdio.h>

// Music Player MCP Module
// This demonstrates how to implement a music player using PWM for speaker control

// Configuration
#define DEFAULT_PWM_PIN 26
#define DEFAULT_VOLUME 50  // 0-100%
#define MAX_NOTE_LENGTH 8  // "C#7:1000"
#define MAX_MELODY_LENGTH 512

// Note frequency table (A4 = 440Hz reference)
typedef struct {
    char note_name[4];  // Store as char array instead of pointer
    float frequency;
} note_freq_t;

// Global pointer to heap-allocated frequency table
static note_freq_t* note_frequencies = NULL;
static int note_count = 68;  // Total notes from C4 to B7

// Initialize note frequency table on heap
static void init_note_frequencies(void) {
    if (note_frequencies != NULL) return;  // Already initialized
    
    // Allocate memory for all notes
    note_frequencies = (note_freq_t*)malloc(note_count * sizeof(note_freq_t));
    if (!note_frequencies) {
        log_error("MUSIC", "Failed to allocate note frequency table");
        return;
    }
    
    int idx = 0;
    
    // C4 octave (Middle C octave) - Using direct initialization instead of strcpy
    note_frequencies[idx] = (note_freq_t){{'C','4','\0'}, 261.63f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'C','#','4','\0'}, 277.18f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','b','4','\0'}, 277.18f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','4','\0'}, 293.66f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','#','4','\0'}, 311.13f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','b','4','\0'}, 311.13f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','4','\0'}, 329.63f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','4','\0'}, 349.23f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','#','4','\0'}, 369.99f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','b','4','\0'}, 369.99f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','4','\0'}, 392.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','#','4','\0'}, 415.30f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','b','4','\0'}, 415.30f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','4','\0'}, 440.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','#','4','\0'}, 466.16f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','b','4','\0'}, 466.16f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','4','\0'}, 493.88f}; idx++;
    
    // C5 octave - Using direct initialization instead of strcpy
    note_frequencies[idx] = (note_freq_t){{'C','5','\0'}, 523.25f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'C','#','5','\0'}, 554.37f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','b','5','\0'}, 554.37f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','5','\0'}, 587.33f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','#','5','\0'}, 622.25f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','b','5','\0'}, 622.25f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','5','\0'}, 659.25f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','5','\0'}, 698.46f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','#','5','\0'}, 739.99f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','b','5','\0'}, 739.99f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','5','\0'}, 783.99f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','#','5','\0'}, 830.61f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','b','5','\0'}, 830.61f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','5','\0'}, 880.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','#','5','\0'}, 932.33f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','b','5','\0'}, 932.33f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','5','\0'}, 987.77f}; idx++;
    
    // C6 octave - Using direct initialization instead of strcpy
    note_frequencies[idx] = (note_freq_t){{'C','6','\0'}, 1046.50f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'C','#','6','\0'}, 1108.73f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','b','6','\0'}, 1108.73f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','6','\0'}, 1174.66f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','#','6','\0'}, 1244.51f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','b','6','\0'}, 1244.51f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','6','\0'}, 1318.51f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','6','\0'}, 1396.91f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','#','6','\0'}, 1479.98f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','b','6','\0'}, 1479.98f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','6','\0'}, 1567.98f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','#','6','\0'}, 1661.22f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','b','6','\0'}, 1661.22f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','6','\0'}, 1760.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','#','6','\0'}, 1864.66f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','b','6','\0'}, 1864.66f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','6','\0'}, 1975.53f}; idx++;
    
    // C7 octave - Using direct initialization instead of strcpy
    note_frequencies[idx] = (note_freq_t){{'C','7','\0'}, 2093.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'C','#','7','\0'}, 2217.46f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','b','7','\0'}, 2217.46f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','7','\0'}, 2349.32f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','#','7','\0'}, 2489.02f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','b','7','\0'}, 2489.02f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','7','\0'}, 2637.02f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','7','\0'}, 2793.83f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','#','7','\0'}, 2959.96f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','b','7','\0'}, 2959.96f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','7','\0'}, 3135.96f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','#','7','\0'}, 3322.44f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','b','7','\0'}, 3322.44f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','7','\0'}, 3520.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','#','7','\0'}, 3729.31f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','b','7','\0'}, 3729.31f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','7','\0'}, 3951.07f}; idx++;
    
    log_info("MUSIC", "Note frequency table initialized on heap");
}

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
    
    // Create human-readable success response
    snprintf(result_json, sizeof(result_json),
        "Playing note %s for %dms at volume %d%% on GPIO pin %d",
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
        "Melody playback not yet implemented. Would play: %s at volume %d%%",
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
        "Music playback stopped on GPIO pin %d", current_pin);
    
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
        "Volume set to %d%%", current_volume);
    
    return mcp_success(result_json);
}

// Helper: Get frequency for a note name
static float get_note_frequency(const char* note) {
    if (!note || !note_frequencies) return 0.0f;
    
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
        .input_schema = "{\"type\":\"object\",\"properties\":{\"note\":{\"type\":\"string\",\"description\":\"Musical note (e.g. 'C4', 'A#5', 'Bb6')\"},\"duration\":{\"type\":\"integer\",\"description\":\"Duration in milliseconds (1-10000)\",\"minimum\":1,\"maximum\":10000},\"volume\":{\"type\":\"integer\",\"description\":\"Volume level (0-100)\",\"minimum\":0,\"maximum\":100}},\"required\":[\"note\"]}",
        .handler = play_note_handler
    },
    {
        .name = "play_melody", 
        .description = "Play a sequence of notes as a melody",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"melody\":{\"type\":\"string\",\"description\":\"Comma-separated musical notes (e.g. 'C4,D4,E4,F4')\"},\"volume\":{\"type\":\"integer\",\"description\":\"Volume level (0-100)\",\"minimum\":0,\"maximum\":100}},\"required\":[\"melody\"]}",
        .handler = play_melody_handler
    },
    {
        .name = "stop_music",
        .description = "Stop current music playback",
        .input_schema = "{\"type\":\"object\",\"properties\":{}}",
        .handler = stop_music_handler
    },
    {
        .name = "set_volume",
        .description = "Set playback volume (0-100)",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"volume\":{\"type\":\"integer\",\"description\":\"Volume level (0-100)\",\"minimum\":0,\"maximum\":100}},\"required\":[\"volume\"]}",
        .handler = set_volume_handler
    }
};

// Module initialization
void _start(void) {
    log_info("MUSIC", "Music Player module starting");
    
    // Initialize note frequency table on heap first
    init_note_frequencies();
    
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