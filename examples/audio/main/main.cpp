#if 0

#include "mcugdx.h"
#include <math.h>

#define TAG "Audio example"

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
	uint8_t value = gpio_read_average(pin, state) / 3154.0f * 255.0f;
	if (value < clamp_value) {
		value = 0;
	}
	return value;
}

int audio_test() {
	mcugdx_init();
	mcugdx_rofs_init();

	mcugdx_gpio_pin_mode(1, MCUGDX_ANALOG_INPUT, MCUGDX_PULL_NONE);
	Average adc_average = {0};
	uint8_t initial_volume = gpio_read_volume(1, &adc_average, 15);

	mcugdx_audio_config_t audio_config = {
			.sample_rate = 44100,
			.channels = MCUGDX_STEREO,
			.bclk = 11,
			.ws = 12,
			.dout = 10};
	mcugdx_audio_init(&audio_config);
	mcugdx_audio_set_master_volume(255);

	mcugdx_log(TAG, "Before load");
	mcugdx_mem_print();

	double start = mcugdx_time();
	mcugdx_sound_t *sound = mcugdx_sound_load("synth.qoa", &mcugdx_rofs, MCUGDX_PRELOADED, MCUGDX_MEM_EXTERNAL);
	mcugdx_log(TAG, "frames: %li, channels: %li, sample rate: %li", sound->num_frames, sound->channels, sound->sample_rate);
	if (sound == NULL) {
		mcugdx_log(TAG, "Failed to load sound");
		return 0;
	}
	mcugdx_log(TAG, "load time: %f", mcugdx_time() - start);
	mcugdx_log(TAG, "After load");
	mcugdx_mem_print();

	// Volume control potentiometer
	mcugdx_sound_id_t synth = mcugdx_sound_play(sound, initial_volume, 127, MCUGDX_LOOP);

	mcugdx_log(TAG, "After play");
	mcugdx_mem_print();

	while (mcugdx_sound_is_playing(synth)) {
		uint8_t value = gpio_read_volume(1, &adc_average, 15);
		mcugdx_log(TAG, "Volume: %d", value);
		mcugdx_sound_set_volume(synth, value);
		mcugdx_sleep(10);
	}

	mcugdx_log(TAG, "Before unload");
	mcugdx_mem_print();

	mcugdx_sound_unload(sound);
	mcugdx_log(TAG, "After unload");
	mcugdx_mem_print();

	return 0;
}

extern "C" int mcugdx_main() {
	return audio_test();
}

#else

#include "mcugdx.h"

#define TAG "Audio example"

extern "C" int mcugdx_main() {
	mcugdx_init();
	mcugdx_rofs_init();

	mcugdx_audio_config_t audio_config = {
			.sample_rate = 44100,
			.channels = MCUGDX_STEREO,
			.bclk = 47,
			.ws = 21,
			.dout = 38};
	mcugdx_audio_init(&audio_config);
	mcugdx_audio_set_master_volume(255);

	mcugdx_log(TAG, "Before load");
	mcugdx_mem_print();

	double start = mcugdx_time();
	mcugdx_sound_t *sound = mcugdx_sound_load("synth.mp3", &mcugdx_rofs, MCUGDX_PRELOADED, MCUGDX_MEM_EXTERNAL);
	mcugdx_log(TAG, "frames: %li, channels: %li, sample rate: %li", sound->num_frames, sound->channels, sound->sample_rate);
	if (sound == NULL) {
		mcugdx_log(TAG, "Failed to load sound");
		return 0;
	}
	mcugdx_log(TAG, "load time: %f", mcugdx_time() - start);

	mcugdx_log(TAG, "After load");
	mcugdx_mem_print();

	mcugdx_sound_id_t synth = mcugdx_sound_play(sound, 255, 127, MCUGDX_SINGLE_SHOT);

	mcugdx_log(TAG, "After play");
	mcugdx_mem_print();

	while (mcugdx_sound_is_playing(synth)) {
        double start = mcugdx_time();
		mcugdx_sleep(1000);
	}
	// mcugdx_sleep(1000);

	mcugdx_log(TAG, "Before unload");
	mcugdx_mem_print();

	mcugdx_sound_unload(sound);
	mcugdx_log(TAG, "After unload");
	mcugdx_mem_print();

	// load the sound, play the sound for 2 seconds, then unload the sound, do this
	// 10 times, then output the memory usage

	for (int i = 0; i < 11; i++) {
		mcugdx_sound_t *sound = mcugdx_sound_load("synth.qoa", &mcugdx_rofs, MCUGDX_PRELOADED, MCUGDX_MEM_EXTERNAL);
		mcugdx_sound_id_t synth = mcugdx_sound_play(sound, 255, 127, MCUGDX_SINGLE_SHOT);
		mcugdx_sleep(1000);
		mcugdx_sound_unload(sound);
	}
	mcugdx_mem_print();

	return 0;
}

#endif
