#include "mcp_sdk.h"
#include <stdio.h>

// Music Player MCP Module
// This demonstrates how to implement a music player using I2S for high-quality audio output

// Configuration
#define DEFAULT_VOLUME 50  // 0-100%
#define MAX_NOTE_LENGTH 8  // "C#7:1000"
#define MAX_MELODY_LENGTH 2048

// Note frequency table (A4 = 440Hz reference)
typedef struct {
    char note_name[4];  // Store as char array instead of pointer
    float frequency;
} note_freq_t;

// Global pointer to heap-allocated frequency table
static note_freq_t* note_frequencies = NULL;
static int note_count = 102;  // Total notes from C2 to B7

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
    
    // C2 octave - Using direct initialization instead of strcpy
    note_frequencies[idx] = (note_freq_t){{'C','2','\0'}, 65.41f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'C','#','2','\0'}, 69.30f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','b','2','\0'}, 69.30f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','2','\0'}, 73.42f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','#','2','\0'}, 77.78f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','b','2','\0'}, 77.78f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','2','\0'}, 82.41f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','2','\0'}, 87.31f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','#','2','\0'}, 92.50f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','b','2','\0'}, 92.50f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','2','\0'}, 98.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','#','2','\0'}, 103.83f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','b','2','\0'}, 103.83f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','2','\0'}, 110.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','#','2','\0'}, 116.54f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','b','2','\0'}, 116.54f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','2','\0'}, 123.47f}; idx++;
    
    // C3 octave - Using direct initialization instead of strcpy
    note_frequencies[idx] = (note_freq_t){{'C','3','\0'}, 130.81f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'C','#','3','\0'}, 138.59f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','b','3','\0'}, 138.59f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','3','\0'}, 146.83f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'D','#','3','\0'}, 155.56f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','b','3','\0'}, 155.56f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'E','3','\0'}, 164.81f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','3','\0'}, 174.61f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'F','#','3','\0'}, 185.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','b','3','\0'}, 185.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','3','\0'}, 196.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'G','#','3','\0'}, 207.65f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','b','3','\0'}, 207.65f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','3','\0'}, 220.00f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'A','#','3','\0'}, 233.08f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','b','3','\0'}, 233.08f}; idx++;
    note_frequencies[idx] = (note_freq_t){{'B','3','\0'}, 246.94f}; idx++;
    
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
    
    // Start I2S for single note
    if (i2s_start() != 0) {
        return mcp_error(-4, "Failed to start I2S");
    }
    
    // Play the note
    mcp_result_t result = play_single_note(note, duration, volume);
    if (result.code != 0) {
        i2s_stop();  // Stop I2S on error
        return result;
    }
    
    // Wait for note to complete (simple blocking)
    for (volatile int i = 0; i < duration * 100; i++) {
        // ~10us per iteration, so duration * 100 ≈ duration in ms
    }
    
    // Stop I2S after note completes
    i2s_stop();
    
    // Create human-readable success response
    snprintf(result_json, sizeof(result_json),
        "Played note %s for %dms at volume %d%% via I2S audio",
        note, duration, volume);
    
    return mcp_success(result_json);
}

// Tool: play_melody - Play a sequence of notes with crossfading
mcp_result_t play_melody_handler(const char* params) {
    char melody[MAX_MELODY_LENGTH];
    int volume = current_volume;
    char result_json[256];
    
    log_info("MUSIC", "play_melody_handler called");

    log_info("MUSIC: params", params);

    // Parse parameters
    if (mcp_parse_string_param(params, "melody", melody, sizeof(melody)) != 0) {
        return mcp_error(-1, "Missing 'melody' parameter");
    }

    log_info("MUSIC", "Melody after wasm parsing ...");
    log_info("MUSIC", melody);

    // Optional volume
    mcp_parse_int_param(params, "volume", &volume);
    
    if (volume < 0 || volume > 100) {
        return mcp_error(-3, "Volume must be between 0 and 100");
    }
    
    log_info("MUSIC", "Converting note names to frequencies");
    
    // Convert note names to frequencies for the new continuous playback
    // Allocate dynamic buffer with 3x expansion factor for frequency conversion
    size_t melody_len = strlen(melody);
    size_t frequency_buffer_size = melody_len * 3 + 1;  // 3x expansion + null terminator
    char* frequency_melody = malloc(frequency_buffer_size);
    if (!frequency_melody) {
        return mcp_error(-5, "Failed to allocate memory for frequency melody buffer");
    }
    frequency_melody[0] = '\0';  // Initialize empty string
    
    char* melody_copy = malloc(melody_len + 1);
    if (!melody_copy) {
        free(frequency_melody);
        return mcp_error(-5, "Failed to allocate memory for melody parsing");
    }
    strcpy(melody_copy, melody);

    log_info("MUSIC", "Melody copy ...");
    log_info("MUSIC", melody_copy);
    
    char* note_token = melody_copy;
    char* next_comma = NULL;
    int first_note = 1;
    
    // Process each note and convert to frequency
    while (note_token && *note_token) {
        // Find next comma or end of string
        next_comma = strstr(note_token, ",");
        if (next_comma) {
            *next_comma = '\0';  // Terminate current note string
        }
        
        // Skip whitespace
        while (*note_token == ' ' || *note_token == '\t') {
            note_token++;
        }
        
        if (*note_token) {  // If we have a note to process
            // Parse note and duration
            char note_name[MAX_NOTE_LENGTH];
            int duration;
            
            if (parse_note_duration(note_token, note_name, &duration) == 0) {
                // Get frequency for the note
                float freq = get_note_frequency(note_name);
                if (freq <= 0.0f) {
                    free(melody_copy);
                    free(frequency_melody);
                    return mcp_error(-6, "Unknown note name");
                }
                
                // Add to frequency melody string with safety checks
                size_t current_len = strlen(frequency_melody);
                char freq_str[32];
                snprintf(freq_str, sizeof(freq_str), "%.2f:%d", freq, duration);
                
                // Calculate space needed
                size_t comma_len = first_note ? 0 : 1;  // comma if not first note
                size_t freq_len = strlen(freq_str);
                size_t total_needed = current_len + comma_len + freq_len + 1; // +1 for null terminator
                
                if (total_needed >= frequency_buffer_size) {
                    free(melody_copy);
                    free(frequency_melody);
                    return mcp_error(-9, "Frequency melody buffer overflow");
                }
                
                if (!first_note) {
                    strcat(frequency_melody, ",");
                }
                strcat(frequency_melody, freq_str);
                
                log_info("MUSIC", "Converted note to frequency");  // Simple debug log
                
                first_note = 0;
            } else {
                free(melody_copy);
                free(frequency_melody);
                return mcp_error(-7, "Invalid note format in melody");
            }
        }
        
        // Move to next note
        if (next_comma) {
            note_token = next_comma + 1;
        } else {
            break;  // No more notes
        }
    }
    
    free(melody_copy);
    
    log_info("MUSIC", "Frequency melody string ready");
    log_info("MUSIC", "Starting continuous melody playback with crossfading");
    
    // Don't call i2s_start() - the melody function handles I2S internally
    // Use the new continuous melody playback with crossfading
    if (i2s_play_melody(frequency_melody, volume) != 0) {
        free(frequency_melody);
        return mcp_error(-8, "Failed to start melody playback");
    }
    
    // Free the frequency melody buffer after successful call to host function
    free(frequency_melody);
    
    is_playing = true;
    
    snprintf(result_json, sizeof(result_json),
        "Playing melody with crossfading at volume %d%% via continuous I2S audio",
        volume);
    
    return mcp_success(result_json);
}

// Tool: stop_music - Stop current playback
mcp_result_t stop_music_handler(const char* params) {
    char result_json[128];
    
    log_info("MUSIC", "stop_music_handler called");
    
    // Stop any type of playback
    if (is_playing) {
        i2s_stop_tone();      // Stop single tone
        i2s_stop_melody();    // Stop melody playback
        i2s_stop();           // Stop I2S clock generation
        is_playing = false;
    }
    
    snprintf(result_json, sizeof(result_json),
        "Music playback stopped via I2S audio");
    
    return mcp_success(result_json);
}

// Tool: test_tone - Generate continuous test tone for oscilloscope debugging
mcp_result_t test_tone_handler(const char* params) {
    int frequency = 1000;  // Default 1kHz
    int volume = 50;       // Default 50%
    char result_json[256];
    
    log_info("MUSIC", "test_tone_handler called");
    
    // Optional frequency parameter
    mcp_parse_int_param(params, "frequency", &frequency);
    mcp_parse_int_param(params, "volume", &volume);
    
    // Validate
    if (frequency < 100 || frequency > 5000) {
        return mcp_error(-1, "Frequency must be between 100 and 5000 Hz");
    }
    
    // Start I2S for test tone
    if (i2s_start() != 0) {
        return mcp_error(-2, "Failed to start I2S for test tone");
    }
    
    // Play continuous tone (duration = 0 means continuous)
    if (i2s_play_tone(frequency, 0, volume) != 0) {
        i2s_stop();
        return mcp_error(-3, "Failed to start test tone");
    }
    
    is_playing = true;
    
    snprintf(result_json, sizeof(result_json),
        "Playing continuous %d Hz test tone at %d%% volume. Use stop_music to stop.",
        frequency, volume);
    
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
    
    // Set I2S volume in real-time
    if (i2s_set_volume(volume) != 0) {
        return mcp_error(-3, "Failed to set I2S volume");
    }
    
    current_volume = volume;
    
    snprintf(result_json, sizeof(result_json),
        "I2S volume set to %d%%", current_volume);
    
    return mcp_success(result_json);
}

// Helper: Get frequency for a note name
static float get_note_frequency(const char* note) {
    if (!note || !note_frequencies) return 0.0f;
    
    for (int i = 0; i < note_count; i++) {
        //if (wasm_strcmp(note, note_frequencies[i].note_name) == 0) {
        if (strcmp(note, note_frequencies[i].note_name) == 0) {
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
    
    //wasm_strncpy(note_out, note_with_duration, note_len);
    strncpy(note_out, note_with_duration, note_len);
    note_out[note_len] = '\0';
    
    // Parse duration
    *duration_out = atoi(colon + 1);
    
    return 0;
}

// Helper: Play a single note using I2S
static mcp_result_t play_single_note(const char* note, int duration, int volume) {
    float frequency = get_note_frequency(note);
    
    if (frequency <= 0.0f) {
        return mcp_error(-4, "Unknown note name");
    }
    
    log_info("MUSIC", "Playing note with I2S");
    
    // Play tone using I2S with precise frequency, duration, and volume
    if (i2s_play_tone((int32_t)frequency, duration, volume) != 0) {
        return mcp_error(-5, "Failed to play I2S tone");
    }
    
    is_playing = true;
    
    return mcp_success("{}");
}

// Bluetooth initialization handler
static mcp_result_t bluetooth_init_handler(const char* params) {
    log_info("MUSIC", "[WASM] Initializing Bluetooth stack");
    
    int32_t result = bluetooth_init();
    if (result != 0) {
        log_error("MUSIC", "[WASM] Failed to initialize Bluetooth");
        return mcp_error(-1, "Failed to initialize Bluetooth");
    }
    
    log_info("MUSIC", "[WASM] Bluetooth initialized successfully");
    return mcp_success("Bluetooth initialized successfully");
}

// Bluetooth scan handler
static mcp_result_t bluetooth_scan_handler(const char* params) {
    log_info("MUSIC", "[WASM] Starting Bluetooth device scan");
    
    // Default timeout 10 seconds
    int32_t timeout = 10;
    
    // Parse timeout from params if provided (simple parsing for now)
    // TODO: Use proper JSON parsing if needed
    
    log_info("MUSIC", "[WASM] Scanning for Bluetooth devices");
    
    // Start scan
    int32_t result = bluetooth_scan_start(timeout);
    if (result != 0) {
        log_error("MUSIC", "[WASM] Failed to start scan");
        return mcp_error(-1, "Failed to start Bluetooth scan");
    }
    
    // Get results (allocate buffer for JSON response)
    char scan_buffer[1024] = {0};
    result = bluetooth_scan_get_results(scan_buffer, sizeof(scan_buffer));
    if (result < 0) {
        log_error("MUSIC", "[WASM] Failed to get scan results");
        return mcp_error(-1, "Failed to get scan results");
    }
    
    log_info("MUSIC", "[WASM] Scan complete");
    
    // Escape JSON for plain text response (replace " with \")
    char escaped_buffer[1024] = {0};
    int src = 0, dst = 0;
    while (scan_buffer[src] && dst < sizeof(escaped_buffer) - 2) {
        if (scan_buffer[src] == '"') {
            escaped_buffer[dst++] = '\\';
            escaped_buffer[dst++] = '"';
        } else {
            escaped_buffer[dst++] = scan_buffer[src];
        }
        src++;
    }
    escaped_buffer[dst] = '\0';
    
    // Return the escaped scan results
    return mcp_success(escaped_buffer);
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
        .input_schema = "{\"type\":\"object\",\"properties\":{\"melody\":{\"type\":\"string\",\"description\":\"Comma-separated musical notes with optional durations (e.g. 'C4:500,D4:300,E4:1000' or 'C4,D4,E4'). Format: NOTE:DURATION_MS where duration is optional (defaults to 500ms)\"},\"volume\":{\"type\":\"integer\",\"description\":\"Volume level (0-100)\",\"minimum\":0,\"maximum\":100}},\"required\":[\"melody\"]}",
        .handler = play_melody_handler
    },
    {
        .name = "stop_music",
        .description = "Stop current music playback",
        .input_schema = "{\"type\":\"object\",\"properties\":{}}",
        .handler = stop_music_handler
    },
    // {
    //     .name = "set_volume",
    //     .description = "Set playback volume (0-100)",
    //     .input_schema = "{\"type\":\"object\",\"properties\":{\"volume\":{\"type\":\"integer\",\"description\":\"Volume level (0-100)\",\"minimum\":0,\"maximum\":100}},\"required\":[\"volume\"]}",
    //     .handler = set_volume_handler
    // },
    // {
    //     .name = "test_tone",
    //     .description = "Generate continuous test tone for debugging (oscilloscope testing)",
    //     .input_schema = "{\"type\":\"object\",\"properties\":{\"frequency\":{\"type\":\"integer\",\"description\":\"Test frequency in Hz (100-5000)\",\"minimum\":100,\"maximum\":5000},\"volume\":{\"type\":\"integer\",\"description\":\"Volume level (0-100)\",\"minimum\":0,\"maximum\":100}}}",
    //     .handler = test_tone_handler
    // },
    {
        .name = "bluetooth_init",
        .description = "Initialize Bluetooth stack for wireless audio output",
        .input_schema = "{\"type\":\"object\",\"properties\":{}}",
        .handler = bluetooth_init_handler
    },
    {
        .name = "bluetooth_scan",
        .description = "Scan for nearby Bluetooth audio devices (speakers/headphones)",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"timeout\":{\"type\":\"integer\",\"description\":\"Scan duration in seconds (1-30)\",\"minimum\":1,\"maximum\":30}}}",
        .handler = bluetooth_scan_handler
    }
};

// Module initialization
void _start(void) {
    log_info("MUSIC", "Music Player module starting");
    
    // Initialize note frequency table on heap first
    init_note_frequencies();
    
    // Initialize I2S audio system (44.1kHz, 16-bit)
    if (i2s_init(44100, 16) != 0) {
        log_error("MUSIC", "Failed to initialize I2S audio system");
        return;
    }
    log_info("MUSIC", "I2S audio system initialized successfully");
    
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