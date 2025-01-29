#include "mcugdx.h"
#include <string.h>

#define TAG "Boxie"
#define MOUNT_POINT "/sdcard"

// SD Card pins (ordered by GPIO number)
#define SD_DAT1_PIN 9 // unused in 1-bit mode
#define SD_DAT0_PIN 10// DATA
#define SD_CLK_PIN 11 // CLK
#define SD_CMD_PIN 12 // CMD
#define SD_CD_PIN 8   // Card Detect (DAT3)
#define SD_DAT2_PIN 14// unused in 1-bit mode

#define READ_BUFFER_SIZE (32 * 1024)// 32KB buffer for reading

static mcugdx_sound_t *current_sound = NULL;
static mcugdx_sound_id_t current_sound_id = -1;

#define WINDOW_SIZE 10

typedef struct {
    int values[WINDOW_SIZE];
    int current_index;
    int samples_collected;
} Average;

// Add this function before mcugdx_main()
int gpio_read_average(int pin, Average* state) {
    state->values[state->current_index] = mcugdx_gpio_analog_in(pin);
    state->current_index = (state->current_index + 1) % WINDOW_SIZE;
    if (state->samples_collected < WINDOW_SIZE) {
        state->samples_collected++;
    }

    int sum = 0;
    for (int i = 0; i < state->samples_collected; i++) {
        sum += state->values[i];
    }
    return sum / state->samples_collected;
}

// Calculates the current volume by reading the current pin state, averaging the last ADC_WINDOW_SIZE samples, and then scaling the result to a 0-255 range.
// This is used to control the volume of the audio playback. Volume below 5 is clamped to 0.
uint8_t gpio_read_volume(int pin, Average* state, int clamp_value) {
	uint8_t value = gpio_read_average(pin, state) / 4096.0f * 255.0f;
	if (value < clamp_value) {
		value = 0;
	}
	return value;
}

// Helper function to find first MP3 file
static bool find_first_mp3(mcugdx_file_system_t fs, mcugdx_file_handle_t dir_handle, char *out_path, size_t path_len) {
	mcugdx_file_handle_t entry;
	while ((entry = fs.read_dir(dir_handle)) != NULL) {
		char name[256];
		fs.file_name(entry, name, sizeof(name));

		// Check files first
		if (!fs.is_dir_handle(entry)) {
			const char *ext = strrchr(name, '.');
			if (ext && strcasecmp(ext, ".mp3") == 0) {
				fs.full_path(entry, out_path, path_len);
				fs.close(entry);
				return true;
			}
		}
		// Then check directories
		else if (fs.is_dir_handle(entry)) {
			if (find_first_mp3(fs, entry, out_path, path_len)) {
				fs.close(entry);
				return true;
			}
		}
		fs.close(entry);
	}
	return false;
}

void print_filesystem_entry(mcugdx_file_system_t fs, mcugdx_file_handle_t handle, int depth) {
	char name[256];
	char indent[32] = {0};

	for (int i = 0; i < depth; i++) {
		strcat(indent, "  ");
	}

	fs.file_name(handle, name, sizeof(name));

	if (fs.is_dir_handle(handle)) {
		mcugdx_log(TAG, "%s[DIR] %s", indent, name);
	} else {
		uint32_t size = fs.length(handle);

		uint8_t *buffer = (uint8_t *) mcugdx_mem_alloc(READ_BUFFER_SIZE, MCUGDX_MEM_EXTERNAL);
		if (buffer) {
			uint32_t total_read = 0;
			uint32_t read_count = 0;
			double start_time = mcugdx_time();

			while (total_read < size) {
				uint32_t to_read = size - total_read;
				if (to_read > READ_BUFFER_SIZE) to_read = READ_BUFFER_SIZE;

				uint32_t bytes_read = fs.read(handle, buffer, to_read);
				if (bytes_read == 0) break;
				total_read += bytes_read;
				read_count++;
			}

			double end_time = mcugdx_time();
			double duration_sec = end_time - start_time;

			// Calculate speeds
			double speed_bytes_per_sec = (double) total_read / duration_sec;
			double speed_KBps = speed_bytes_per_sec / 1024.0;
			double speed_kbps = speed_bytes_per_sec * 8.0 / 1000.0;

			mcugdx_log(TAG, "%s[FILE] %s", indent, name);
			mcugdx_log(TAG, "%s      Size: %lu bytes", indent, (unsigned long) size);
			mcugdx_log(TAG, "%s      Read: %lu bytes in %d chunks", indent,
					   (unsigned long) total_read, read_count);
			mcugdx_log(TAG, "%s      Time: %.3f seconds", indent, duration_sec);
			mcugdx_log(TAG, "%s      Speed: %.2f KB/s (%.2f kb/s)",
					   indent, speed_KBps, speed_kbps);

			mcugdx_mem_free(buffer);
		}
	}
}

void traverse_directory(mcugdx_file_system_t fs, mcugdx_file_handle_t dir_handle, int depth) {
	mcugdx_file_handle_t entry;

	while ((entry = fs.read_dir(dir_handle)) != NULL) {
		print_filesystem_entry(fs, entry, depth);

		if (fs.is_dir_handle(entry)) {
			traverse_directory(fs, entry, depth + 1);
		}

		fs.close(entry);
	}
}

extern "C" int mcugdx_main() {
	mcugdx_init();
	mcugdx_log(TAG, "Initializing SD card system...");

	// Initialize audio
	mcugdx_audio_config_t audio_config = {
			.sample_rate = 44100,
			.channels = MCUGDX_STEREO,
			.bclk = 5,
			.ws = 6,
			.dout = 4};
	mcugdx_audio_init(&audio_config);
	mcugdx_audio_set_master_volume(255);

	mcugdx_sdfs_config_t config = {
			.pin_clk = SD_CLK_PIN,
			.pin_cmd = SD_CMD_PIN,
			.pin_d0 = SD_DAT0_PIN,
			.pin_cd = SD_CD_PIN,
			.mount_path = MOUNT_POINT};

	// Initialize SD system to set up CD pin
	bool sd_mounted = mcugdx_sdfs_init(&config);
	mcugdx_log(TAG, "Initial card state: %s", mcugdx_sdfs_is_card_present() ? "PRESENT" : "NOT PRESENT");

	if (sd_mounted) {
		mcugdx_log(TAG, "SD card mounted successfully");

		// First try to play synth.mp3 directly
		if (mcugdx_sdfs.exists("synth.mp3")) {
			mcugdx_log(TAG, "Found MP3: synth.mp3");
			current_sound = mcugdx_sound_load("synth.mp3", &mcugdx_sdfs, MCUGDX_PRELOADED, MCUGDX_MEM_EXTERNAL);
			if (current_sound) {
				current_sound_id = mcugdx_sound_play(current_sound, 1, 127, MCUGDX_LOOP);
			}
		} else {
			mcugdx_log(TAG, "synth.mp3 not found");
		}

		// Then traverse and print the filesystem contents
		mcugdx_file_handle_t root = mcugdx_sdfs.open_root();
		if (root) {
			mcugdx_log(TAG, "Filesystem contents:");
			traverse_directory(mcugdx_sdfs, root, 0);
			mcugdx_sdfs.close(root);
		}
	}

	bool last_card_state = mcugdx_sdfs_is_card_present();
	bool currently_mounted = sd_mounted;

	while (1) {
		bool card_present = mcugdx_sdfs_is_card_present();
		if (card_present != last_card_state) {
			if (card_present) {
				if (mcugdx_sdfs_init(&config)) {
					mcugdx_log(TAG, "SD card mounted successfully");

					// First try to play synth.mp3 directly
					if (mcugdx_sdfs.exists("synth.mp3")) {
						mcugdx_log(TAG, "Found MP3: synth.mp3");
						current_sound = mcugdx_sound_load("synth.mp3", &mcugdx_sdfs, MCUGDX_PRELOADED, MCUGDX_MEM_EXTERNAL);
						if (current_sound) {
							current_sound_id = mcugdx_sound_play(current_sound, 1, 127, MCUGDX_LOOP);
						}
					} else {
						mcugdx_log(TAG, "synth.mp3 not found");
					}

					// Then traverse and print the filesystem contents
					mcugdx_file_handle_t root = mcugdx_sdfs.open_root();
					if (root) {
						mcugdx_log(TAG, "Filesystem contents:");
						traverse_directory(mcugdx_sdfs, root, 0);
						mcugdx_sdfs.close(root);
					}
				}
			} else {
				mcugdx_log(TAG, "Card removed");
				if (currently_mounted) {
					if (current_sound) {
						mcugdx_sound_stop(current_sound_id);
						mcugdx_sound_unload(current_sound);
						current_sound = NULL;
						current_sound_id = -1;
					}
					mcugdx_sdfs_deinit();
					currently_mounted = false;
				}
			}
			last_card_state = card_present;
		}
		mcugdx_sleep(100);
	}

	return 0;
}
