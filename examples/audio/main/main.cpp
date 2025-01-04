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

	return 0;
}
