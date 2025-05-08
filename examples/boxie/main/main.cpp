#include "mcugdx.h"
#include <string.h>
#include <math.h>
#include "esp_system.h"

#define TAG "Boxie"
#define MOUNT_POINT "/sdcard"

// SD Card pins (ordered by GPIO number)
#define SD_DAT1_PIN 9 // unused in 1-bit mode
#define SD_DAT0_PIN 10// DATA
#define SD_CLK_PIN 11 // CLK
#define SD_CMD_PIN 12 // CMD
#define SD_CD_PIN 13  // Card Detect (DAT3)
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

#define MAX_VOLUME 128 // Maximum volume level (0-255)

// Add this function before mcugdx_main()
int gpio_read_average(int pin, Average *state) {
	int raw_value = mcugdx_gpio_analog_in(pin);
	if (raw_value > 4096) raw_value = 4096;

	state->values[state->current_index] = raw_value;
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

// Calculates the current volume by reading the current pin state, averaging the last ADC_WINDOW_SIZE samples, and then scaling the result to 0-MAX_VOLUME.
// Volume below 5 is clamped to 0.
uint8_t gpio_read_volume(int pin, Average *state, int clamp_value) {
	float raw_value = gpio_read_average(pin, state) / 4096.0f;// Normalize to 0-1

	uint8_t value = raw_value * MAX_VOLUME;// Scale to 0-MAX_VOLUME
	if (value < clamp_value) {
		value = 0;
	}
	return value;
}

void traverse_directory(mcugdx_file_system_t fs, mcugdx_file_handle_t dir_handle, int depth) {
	mcugdx_file_handle_t entry;

	while ((entry = fs.read_dir(dir_handle)) != NULL) {
		char name[256];
		char indent[32] = {0};

		for (int i = 0; i < depth; i++) {
			strcat(indent, "  ");
		}

		fs.file_name(entry, name, sizeof(name));

		if (fs.is_dir_handle(entry)) {
			mcugdx_log(TAG, "%s[DIR] %s", indent, name);
		} else {
			mcugdx_log(TAG, "%s[FILE] %s", indent, name);
			uint32_t size = fs.length(entry);
			mcugdx_log(TAG, "%s      Size: %lu bytes", indent, (unsigned long) size);
		}

		if (fs.is_dir_handle(entry)) {
			traverse_directory(fs, entry, depth + 1);
		}

		fs.close(entry);
		mcugdx_log(TAG, "Closing entry");
		mcugdx_mem_print();
	}
}

// Button and volume control pins
#define LEFT_BUTTON_PIN 7
#define RIGHT_BUTTON_PIN 16
#define VOLUME_POT_PIN 15

// Button handles
static mcugdx_button_handle_t left_button;
static mcugdx_button_handle_t right_button;
static Average volume_average = {0};// For volume smoothing

// Add this with other static variables at the top
static uint8_t last_volume = 0;

#define MAX_MP3_FILES 32// Maximum number of MP3 files to store

typedef struct {
	char **filenames;// Dynamic array of filename strings
	int count;       // Number of files found
	int capacity;    // Current capacity of the array
} mp3_file_list_t;

static void free_mp3s(mp3_file_list_t *list);

// Helper function for qsort
static int compare_filenames(const void* a, const void* b) {
	const char* str1 = *(const char**)a;
	const char* str2 = *(const char**)b;
	return strcasecmp(str1, str2);
}

static mp3_file_list_t* list_mp3s(mcugdx_file_system_t fs) {
	mp3_file_list_t *list = (mp3_file_list_t *) mcugdx_mem_alloc(sizeof(mp3_file_list_t), MCUGDX_MEM_EXTERNAL);
	if (!list) {
		mcugdx_log(TAG, "Failed to allocate list structure");
		return NULL;
	}

	list->count = 0;
	list->capacity = 16;// Initial capacity
	list->filenames = (char **) mcugdx_mem_alloc(list->capacity * sizeof(char *), MCUGDX_MEM_EXTERNAL);
	if (!list->filenames) {
		mcugdx_log(TAG, "Failed to allocate filenames array");
		mcugdx_mem_free(list);
		return NULL;
	}

	mcugdx_file_handle_t root = fs.open_root();
	if (!root) {
		mcugdx_log(TAG, "Failed to open root directory");
		free_mp3s(list);
		return NULL;
	}

	mcugdx_file_handle_t entry;
	while ((entry = fs.read_dir(root)) != NULL) {
		if (!fs.is_dir_handle(entry)) {
			char name[256];
			fs.file_name(entry, name, sizeof(name));

			const char *ext = strrchr(name, '.');
			if (ext && (strcasecmp(ext, ".mp3") == 0)) {
				// Grow array if needed
				if (list->count == list->capacity) {
					int new_capacity = list->capacity * 2;
					char **new_filenames = (char **) mcugdx_mem_alloc(new_capacity * sizeof(char *), MCUGDX_MEM_EXTERNAL);
					if (!new_filenames) {
						mcugdx_log(TAG, "Failed to grow filenames array");
						fs.close(entry);
						fs.close(root);
						free_mp3s(list);
						return NULL;
					}

					// Copy existing pointers
					for (int i = 0; i < list->count; i++) {
						new_filenames[i] = list->filenames[i];
					}

					mcugdx_mem_free(list->filenames);
					list->filenames = new_filenames;
					list->capacity = new_capacity;
				}

				// Allocate and copy filename
				list->filenames[list->count] = (char *) mcugdx_mem_alloc(strlen(name) + 1, MCUGDX_MEM_EXTERNAL);
				if (!list->filenames[list->count]) {
					mcugdx_log(TAG, "Failed to allocate filename string");
					fs.close(entry);
					fs.close(root);
					free_mp3s(list);
					return NULL;
				}
				strcpy(list->filenames[list->count], name);
				list->count++;
			}
		}
		fs.close(entry);
	}

	fs.close(root);

	// Sort the filenames before returning
	if (list->count > 0) {
		qsort(list->filenames, list->count, sizeof(char*), compare_filenames);
	}

	// Add logging of found files
	mcugdx_log(TAG, "Found %d MP3 files:", list->count);
	for (int i = 0; i < list->count; i++) {
		mcugdx_log(TAG, "[%d] %s", i, list->filenames[i]);
	}

	return list;
}

static void free_mp3s(mp3_file_list_t *list) {
	if (!list) return;

	if (list->filenames) {
		// Free each filename string
		for (int i = 0; i < list->count; i++) {
			if (list->filenames[i]) {
				mcugdx_mem_free(list->filenames[i]);
			}
		}
		// Free the array of pointers
		mcugdx_mem_free(list->filenames);
	}

	// Free the list structure itself
	mcugdx_mem_free(list);
}

static void print_mp3s() {
	mp3_file_list_t *mp3_files = list_mp3s(mcugdx_sdfs);
	if (mp3_files) {
		mcugdx_log(TAG, "Found %d MP3 files", mp3_files->count);
		for (int i = 0; i < mp3_files->count; i++) {
			mcugdx_log(TAG, "Found MP3: %s", mp3_files->filenames[i]);
		}

		// When done with the list
		free_mp3s(mp3_files);
	}
}

// Add these static variables at the top with the other static declarations
static mp3_file_list_t* current_mp3_list = NULL;
static int current_mp3_index = 0;

// Helper function to play an MP3 by index
static void play_mp3_at_index(int index) {
	if (!current_mp3_list || current_mp3_list->count == 0) return;

	// Stop and unload current sound if it exists
	if (current_sound) {
		mcugdx_sound_stop(current_sound_id);
		// Wait for sound to stop
		while (mcugdx_sound_is_playing(current_sound_id)) {
			mcugdx_sleep(1);
		}
		mcugdx_sound_unload(current_sound);
		current_sound = NULL;
		current_sound_id = -1;
	}

	// Load and play new sound
	current_sound = mcugdx_sound_load(current_mp3_list->filenames[index], &mcugdx_sdfs,
									MCUGDX_PRELOADED, MCUGDX_MEM_EXTERNAL);
	if (current_sound) {
		current_sound_id = mcugdx_sound_play(current_sound, 255, 127, MCUGDX_SINGLE_SHOT);
		mcugdx_log(TAG, "Playing [%d/%d]: %s",
				  index + 1, current_mp3_list->count,
				  current_mp3_list->filenames[index]);
	} else {
		mcugdx_log(TAG, "Failed to load sound [%d/%d]: %s",
				  index + 1, current_mp3_list->count,
				  current_mp3_list->filenames[index]);

		// Advance to next track
		current_mp3_index = (index + 1) % current_mp3_list->count;
		play_mp3_at_index(current_mp3_index);
	}
}

extern "C" int mcugdx_main() {
	mcugdx_init();

	// Initialize buttons with 50ms debounce time
	left_button = mcugdx_button_create(LEFT_BUTTON_PIN, 50, MCUGDX_KEY_LEFT);
	right_button = mcugdx_button_create(RIGHT_BUTTON_PIN, 50, MCUGDX_KEY_RIGHT);

	// Initialize volume potentiometer as analog input
	mcugdx_gpio_pin_mode(VOLUME_POT_PIN, MCUGDX_ANALOG_INPUT, MCUGDX_PULL_NONE);

	// Initialize audio
	mcugdx_audio_config_t audio_config = {
			.sample_rate = 44100,
			.channels = MCUGDX_STEREO,
			.bclk = 5,
			.ws = 6,
			.dout = 4};
	mcugdx_audio_init(&audio_config);
	mcugdx_audio_set_master_volume(255);

	// Set initial volume based on potentiometer
	uint8_t volume = gpio_read_volume(VOLUME_POT_PIN, &volume_average, 1);
	mcugdx_audio_set_master_volume(volume);
	last_volume = volume;
	mcugdx_log(TAG, "Initial volume: %d", volume);

		// Initialize both filesystems
	if (!mcugdx_rofs_init()) {
		mcugdx_log(TAG, "Failed to initialize ROFS");
		return -1;
	}
	mcugdx_sound_t *startup_sound = mcugdx_sound_load("startup.mp3", &mcugdx_rofs, MCUGDX_PRELOADED, MCUGDX_MEM_EXTERNAL);
	mcugdx_sound_id_t startup_sound_id = mcugdx_sound_play(startup_sound, 255, 127, MCUGDX_SINGLE_SHOT);
	while(mcugdx_sound_is_playing(startup_sound_id)) {
		mcugdx_sleep(100);
	}
	mcugdx_sleep(500);

	mcugdx_log(TAG, "Initializing SD card system...");
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

		// Get list of MP3s
		current_mp3_list = list_mp3s(mcugdx_sdfs);
		if (current_mp3_list && current_mp3_list->count > 0) {
			// Start playing first MP3
			current_mp3_index = 0;
			play_mp3_at_index(current_mp3_index);
		}
	} else {
		mcugdx_log(TAG, "Could not mount SD card, restarting after 1s pause");
		mcugdx_sleep(5000);
		esp_restart();
	}

	bool last_card_state = mcugdx_sdfs_is_card_present();
	bool currently_mounted = sd_mounted;

	while (1) {
		// Handle volume control
		uint8_t volume = gpio_read_volume(VOLUME_POT_PIN, &volume_average, 1);
		if (volume != last_volume) {
			mcugdx_log(TAG, "Volume changed: %d", volume);
			last_volume = volume;
		}
		mcugdx_audio_set_master_volume(volume);

		// Handle button events
		mcugdx_button_event_t button_event;
		while (mcugdx_button_get_event(&button_event)) {
			if (button_event.type == MCUGDX_BUTTON_PRESSED && current_mp3_list) {
				if (button_event.keycode == MCUGDX_KEY_RIGHT) {
					// Next track
					current_mp3_index = (current_mp3_index + 1) % current_mp3_list->count;
					play_mp3_at_index(current_mp3_index);
				} else if (button_event.keycode == MCUGDX_KEY_LEFT) {
					// Previous track
					current_mp3_index = (current_mp3_index - 1 + current_mp3_list->count) % current_mp3_list->count;
					play_mp3_at_index(current_mp3_index);
				}
			}
		}

		// Handle card presence changes
		bool card_present = mcugdx_sdfs_is_card_present();
		if (card_present != last_card_state) {
			if (card_present) {
				if (mcugdx_sdfs_init(&config)) {
					mcugdx_log(TAG, "SD card mounted successfully");

					// Get new list of MP3s
					current_mp3_list = list_mp3s(mcugdx_sdfs);
					if (current_mp3_list && current_mp3_list->count > 0) {
						// Start playing first MP3
						current_mp3_index = 0;
						play_mp3_at_index(current_mp3_index);
					}
				}
			} else {
				mcugdx_log(TAG, "Card removed");
				mcugdx_log(TAG, "Restarting ESP32...");
				mcugdx_sleep(1000);  // Wait 1 second before restarting
				esp_restart();  // Restart the ESP32
			}
			last_card_state = card_present;
		}

		// Check if current sound finished playing and start next track
		if (current_sound && !mcugdx_sound_is_playing(current_sound_id)) {
			current_mp3_index = (current_mp3_index + 1) % current_mp3_list->count;
			play_mp3_at_index(current_mp3_index);
		}

		mcugdx_sleep(16);
	}

	return 0;
}
