#include "audio.h"
#include "log.h"
#include "mutex.h"
#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include "thirdparty/qoa.h"
#include <string.h>
#include <mcugdx.h>
#include <math.h>
#include "helix_mp3.h"

#define TAG "mcugdx_audio"
#define MAX_SOUND_INSTANCES 32

static inline int32_t *mix_frames(int32_t *output, int32_t channels, int32_t left_sample, int32_t right_sample, int32_t pan_left_gain, int32_t pan_right_gain, int32_t final_gain) {
	left_sample = ((left_sample * pan_left_gain) >> 8) * final_gain >> 8;
	right_sample = ((right_sample * pan_right_gain) >> 8) * final_gain >> 8;

	if (channels == MCUGDX_MONO) {
		output[0] += (left_sample + right_sample) >> 1;
		return output + 1;
	} else {
		output[0] += left_sample;
		output[1] += right_sample;
		return output + 2;
	}
}

typedef struct mcugdx_sound_internal_t mcugdx_sound_internal_t;
typedef struct mcugdx_audio_renderer_t mcugdx_audio_renderer_t;

typedef uint32_t (*render_fn)(mcugdx_sound_internal_t *sound, mcugdx_audio_renderer_t *renderer, int32_t *buffer, uint32_t num_frames, mcugdx_audio_channels_t channels, int32_t pan_left_gain, int32_t pan_right_gain, int32_t final_gain);
typedef void (*reset_fn)(mcugdx_sound_internal_t *sound, mcugdx_audio_renderer_t *renderer);
typedef void (*free_fn)(mcugdx_sound_internal_t *sound, mcugdx_audio_renderer_t *renderer);

struct mcugdx_audio_renderer_t {
	render_fn render;
	reset_fn reset;
	free_fn free;
	void *renderer_data;
};

struct mcugdx_sound_internal_t {
	mcugdx_sound_t sound;
	mcugdx_audio_renderer_t *(*create_renderer)(mcugdx_sound_internal_t *sound, void *renderer_data);
	void *renderer_data;
};

typedef struct {
	mcugdx_sound_internal_t *sound;
	mcugdx_audio_renderer_t *renderer;
	uint8_t volume;
	uint8_t pan;
	mcugdx_playback_mode_t mode;
	uint32_t id;
} mcugdx_sound_instance_t;

static mcugdx_sound_instance_t sound_instances[MAX_SOUND_INSTANCES] = {0};
static uint32_t next_id = 0;
static uint8_t master_volume = 255;
extern mcugdx_mutex_t audio_lock;

// Preloaded sound renderer implementation
typedef struct {
	int16_t *frames;
	uint32_t num_frames;
	uint32_t position;
} preloaded_renderer_data_t;

static uint32_t preloaded_render(mcugdx_sound_internal_t *sound, mcugdx_audio_renderer_t *renderer, int32_t *buffer, uint32_t num_frames, mcugdx_audio_channels_t channels, int32_t pan_left_gain, int32_t pan_right_gain, int32_t final_gain) {
	preloaded_renderer_data_t *data = renderer->renderer_data;
	uint32_t frames_remaining = data->num_frames - data->position;
	uint32_t frames_to_render = num_frames < frames_remaining ? num_frames : frames_remaining;

	if (frames_to_render == 0) return 0;

	int32_t *output = buffer;
	for (uint32_t i = 0; i < frames_to_render; i++) {
		int32_t left_sample, right_sample;
		if (sound->sound.channels == 1) {
			int32_t mono_sample = data->frames[data->position];
			left_sample = right_sample = mono_sample;
		} else {
			left_sample = data->frames[data->position * 2];
			right_sample = data->frames[data->position * 2 + 1];
		}

		output = mix_frames(output, channels, left_sample, right_sample, pan_left_gain, pan_right_gain, final_gain);
		data->position++;
	}
	return frames_to_render;
}

static void preloaded_reset(mcugdx_sound_internal_t *sound, mcugdx_audio_renderer_t *renderer) {
	preloaded_renderer_data_t *data = renderer->renderer_data;
	data->position = 0;
}

static void preloaded_free(mcugdx_sound_internal_t *sound, mcugdx_audio_renderer_t *renderer) {
	// Renderer data is stored on sound and shared between instances, no need to free it.
	// mcugdx_mem_free(renderer->renderer_data);
	mcugdx_mem_free(renderer);
}

static mcugdx_audio_renderer_t *create_preloaded_renderer(mcugdx_sound_internal_t *sound, void *renderer_data) {
	preloaded_renderer_data_t *data = renderer_data;
	mcugdx_audio_renderer_t *renderer = mcugdx_mem_alloc(sizeof(mcugdx_audio_renderer_t), MCUGDX_MEM_EXTERNAL);

	renderer->render = preloaded_render;
	renderer->reset = preloaded_reset;
	renderer->free = preloaded_free;
	renderer->renderer_data = data;

	return renderer;
}

/*// Streaming renderer implementation
typedef struct {
	// FIXME
	void *data;
} streaming_renderer_data_t;

static uint32_t streaming_render(mcugdx_audio_renderer_t *renderer, int16_t *buffer, uint32_t num_frames) {
	(void) renderer;
	(void) buffer;
	(void) num_frames;
	// FIXME
	return 0;
}

static void streaming_reset(mcugdx_audio_renderer_t *renderer) {
	(void) renderer;
	// FIXME
}

static void streaming_free(mcugdx_audio_renderer_t *renderer) {
	(void) renderer;
	// FIXME
}

static mcugdx_audio_renderer_t *create_streaming_renderer(mcugdx_sound_t *sound, void *renderer_data) {
	(void) sound;
	(void) renderer_data;
	// FIXME
	return NULL;
}*/

typedef struct {
	void (*decode_frame)(const uint8_t *data, uint32_t size, void *decoder_data, int16_t *output, uint32_t *num_frames);
	void (*free_decoder)(void *decoder_data);
	void *decoder_data;
} mcugdx_audio_decoder_t;

typedef struct {
	const char *extension;
	bool (*init_streaming)(const char *path, mcugdx_file_system_t *fs, mcugdx_memory_type_t mem_type,
						   mcugdx_audio_decoder_t *decoder, uint32_t *first_frame_pos,
						   uint32_t *sample_rate, uint32_t *channels, uint32_t *num_frames,
						   uint32_t *buffer_size, uint32_t *frames_size);
	bool (*init_preloaded)(const char *path, mcugdx_file_system_t *fs, mcugdx_memory_type_t mem_type,
						   int16_t **frames, uint32_t *sample_rate, uint32_t *channels, uint32_t *num_frames);
} mcugdx_audio_format_t;

static bool qoa_init_preloaded(const char *path, mcugdx_file_system_t *fs, mcugdx_memory_type_t mem_type,
							   int16_t **frames, uint32_t *sample_rate, uint32_t *channels, uint32_t *num_frames) {
	uint32_t size;
	uint8_t *raw = fs->read_fully(path, &size, MCUGDX_MEM_EXTERNAL);
	if (!raw) return false;

	qoa_desc qoa;
	if (!qoa_decode_header(raw, size, &qoa)) {
		mcugdx_mem_free(raw);
		return false;
	}

	*frames = qoa_decode(raw, size, &qoa, mem_type);
	*sample_rate = qoa.samplerate;
	*channels = qoa.channels;
	*num_frames = qoa.samples;
	mcugdx_mem_free(raw);
	return *frames != NULL;
}

// Helper struct to hold file system context
typedef struct {
	mcugdx_file_system_t *fs;
	mcugdx_file_handle_t handle;
} mcugdx_file_io_t;

// File-based seek function compatible with helix_mp3_io_t
static int mcugdx_file_seek(void* ctx, int offset) {
	mcugdx_file_io_t *io = (mcugdx_file_io_t*)ctx;
	return io->fs->seek(io->handle, offset) ? 0 : -1;
}

// File-based read function compatible with helix_mp3_io_t
static size_t mcugdx_file_read(void* ctx, void* buffer, size_t size) {
	mcugdx_file_io_t *io = (mcugdx_file_io_t*)ctx;
	return io->fs->read(io->handle, buffer, size);
}

static bool mp3_init_preloaded(const char *path, mcugdx_file_system_t *fs, mcugdx_memory_type_t mem_type,
							   int16_t **frames, uint32_t *sample_rate, uint32_t *channels, uint32_t *num_frames) {
	// Setup file IO for helix
	helix_mp3_t mp3;
	helix_mp3_io_t io = {0};

	// Open the file
	mcugdx_file_handle_t handle = fs->open(path);
	if (!handle) return false;

	// Setup file IO context
	mcugdx_file_io_t file_io = { fs, handle };

	io.seek = mcugdx_file_seek;
	io.read = mcugdx_file_read;
	io.user_data = &file_io;

	// Initialize decoder
	if (helix_mp3_init(&mp3, &io) != 0) {
		fs->close(handle);
		return false;
	}

	// First pass: count total frames, this is super bad, it decodes the entire file...
	size_t total_frames = 0;
	int16_t temp_buffer[HELIX_MP3_MAX_SAMPLES_PER_FRAME * 2]; // Keep buffer size for worst case (stereo)

	while (true) {
		size_t frames = helix_mp3_read_pcm_frames_s16(&mp3, temp_buffer, HELIX_MP3_MAX_SAMPLES_PER_FRAME);
		if (frames == 0) break;
		total_frames += frames;
	}

	// Get the actual channel count
	uint8_t mp3_channels = helix_mp3_get_channels(&mp3);

	// Allocate buffer for all frames
	size_t buffer_size = total_frames * mp3_channels * sizeof(int16_t); // Use actual channel count
	*frames = mcugdx_mem_alloc(buffer_size, mem_type);
	if (!*frames) {
		helix_mp3_deinit(&mp3);
		fs->close(handle);
		return false;
	}

	// Reset file and decoder
	fs->seek(handle, 0);
	helix_mp3_deinit(&mp3);
	if (helix_mp3_init(&mp3, &io) != 0) {
		mcugdx_mem_free(*frames);
		fs->close(handle);
		return false;
	}

	// Second pass: decode all frames
	size_t frames_read = 0;
	int16_t *output_ptr = *frames;  // Track our position in the output buffer

	while (frames_read < total_frames) {
		size_t frames_to_read = total_frames - frames_read;

		// Read directly into the current output buffer position
		size_t num_frames = helix_mp3_read_pcm_frames_s16(&mp3,
			output_ptr,
			frames_to_read);

		if (num_frames == 0) break;

		// Advance the output pointer by the number of samples written
		output_ptr += (num_frames * mp3_channels);
		frames_read += num_frames;
	}

	*sample_rate = helix_mp3_get_sample_rate(&mp3);
	*channels = mp3_channels;  // Use actual channel count
	*num_frames = frames_read;

	helix_mp3_deinit(&mp3);
	fs->close(handle);
	return true;
}

static const mcugdx_audio_format_t formats[] = {
		{".qoa", NULL, qoa_init_preloaded},
		{".mp3", NULL, mp3_init_preloaded}, // Add MP3 support
		{NULL, NULL, NULL}};

mcugdx_sound_t *mcugdx_sound_load(const char *path, mcugdx_file_system_t *fs,
								  mcugdx_sound_type_t sound_type, mcugdx_memory_type_t mem_type) {
	if (!path || !fs) {
		mcugdx_loge(TAG, "Invalid parameters");
		return NULL;
	}

	// Find matching format handler
	const mcugdx_audio_format_t *format = NULL;
	const char *extension = strrchr(path, '.');
	if (!extension) {
		mcugdx_loge(TAG, "No file extension found");
		return NULL;
	}

	for (int i = 0; formats[i].extension != NULL; i++) {
		if (strcasecmp(extension, formats[i].extension) == 0) {
			format = &formats[i];
			break;
		}
	}

	if (!format) {
		mcugdx_loge(TAG, "Unsupported audio format: %s", extension);
		return NULL;
	}

	mcugdx_sound_internal_t *internal = mcugdx_mem_alloc(sizeof(mcugdx_sound_internal_t), mem_type);
	if (!internal) {
		mcugdx_loge(TAG, "Failed to allocate sound internal structure");
		return NULL;
	}

	if (sound_type == MCUGDX_PRELOADED) {
		if (!format->init_preloaded) {
			mcugdx_loge(TAG, "Format doesn't support preloaded playback");
			mcugdx_mem_free(internal);
			return NULL;
		}

		preloaded_renderer_data_t *renderer_data = mcugdx_mem_alloc(sizeof(preloaded_renderer_data_t), mem_type);
		if (!renderer_data) {
			mcugdx_mem_free(internal);
			return NULL;
		}

		uint32_t num_frames;
		if (!format->init_preloaded(path, fs, mem_type, &renderer_data->frames,
									&internal->sound.sample_rate, &internal->sound.channels, &num_frames)) {
			mcugdx_mem_free(renderer_data);
			mcugdx_mem_free(internal);
			return NULL;
		}

		internal->sound.num_frames = renderer_data->num_frames = num_frames;
		renderer_data->position = 0;
		internal->create_renderer = create_preloaded_renderer;
		internal->sound.type = MCUGDX_PRELOADED;
		internal->renderer_data = renderer_data;
	} else {
		// Streaming implementation would go here
		mcugdx_loge(TAG, "Streaming not yet implemented");
		mcugdx_mem_free(internal);
		return NULL;
	}

	return &internal->sound;
}

void mcugdx_sound_unload(mcugdx_sound_t *sound) {
	if (!sound) return;

	mcugdx_sound_internal_t *internal = (mcugdx_sound_internal_t *) sound;

	// Stop any playing instances of this sound
	mcugdx_mutex_lock(&audio_lock);
	for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
		if (sound_instances[i].sound == internal) {
			if (sound_instances[i].renderer) {
				sound_instances[i].renderer->free(internal, sound_instances[i].renderer);
			}
			memset(&sound_instances[i], 0, sizeof(mcugdx_sound_instance_t));
		}
	}
	mcugdx_mutex_unlock(&audio_lock);

	// Free the sound data
	if (sound->type == MCUGDX_PRELOADED) {
		preloaded_renderer_data_t *data = internal->renderer_data;
		if (data) {
			if (data->frames) {
				mcugdx_mem_free(data->frames);
			}
			mcugdx_mem_free(data);
		}
	}

	mcugdx_mem_free(internal);
}

double mcugdx_sound_duration(mcugdx_sound_t *sound) {
	mcugdx_sound_internal_t *internal = (mcugdx_sound_internal_t *) sound;
	// FIXME
	return 0;
}

mcugdx_sound_id_t mcugdx_sound_play(mcugdx_sound_t *sound, uint8_t volume, uint8_t pan, mcugdx_playback_mode_t mode) {
	mcugdx_mutex_lock(&audio_lock);
	mcugdx_sound_instance_t *free_slot = NULL;
	mcugdx_sound_id_t free_slot_idx = 0;
	mcugdx_sound_instance_t *lowest_id_slot = &sound_instances[0];
	mcugdx_sound_id_t lowest_id_slot_idx = 0;

	for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
		if (sound_instances[i].sound == NULL) {
			free_slot = &sound_instances[i];
			free_slot_idx = i;
			break;
		}
		if (sound_instances[i].id < lowest_id_slot->id) {
			lowest_id_slot = &sound_instances[i];
			lowest_id_slot_idx = i;
		}
	}

	mcugdx_sound_instance_t *instance = free_slot ? free_slot : lowest_id_slot;
	mcugdx_sound_id_t id = free_slot ? free_slot_idx : lowest_id_slot_idx;

	mcugdx_sound_internal_t *internal = (mcugdx_sound_internal_t *) sound;
	instance->sound = (mcugdx_sound_internal_t *) sound;
	instance->volume = volume;
	instance->pan = pan;
	instance->mode = mode;
	instance->id = ++next_id;
	instance->renderer = internal->create_renderer(internal, internal->renderer_data);

	mcugdx_mutex_unlock(&audio_lock);
	return id;
}

void mcugdx_sound_set_volume(mcugdx_sound_id_t sound_instance, uint8_t volume) {
	if (sound_instance > MAX_SOUND_INSTANCES) return;
	mcugdx_mutex_lock(&audio_lock);
	mcugdx_sound_instance_t *instance = &sound_instances[sound_instance];
	if (!instance->sound) {
		mcugdx_mutex_unlock(&audio_lock);
		return;
	}
	sound_instances[sound_instance].volume = volume;
	mcugdx_mutex_unlock(&audio_lock);
}

void mcugdx_sound_set_pan(mcugdx_sound_id_t sound_instance, uint8_t pan) {
	if (sound_instance > MAX_SOUND_INSTANCES) return;
	mcugdx_mutex_lock(&audio_lock);
	mcugdx_sound_instance_t *instance = &sound_instances[sound_instance];
	if (!instance->sound) {
		mcugdx_mutex_unlock(&audio_lock);
		return;
	}
	sound_instances[sound_instance].pan = pan;
	mcugdx_mutex_unlock(&audio_lock);
}

void mcugdx_sound_stop(mcugdx_sound_id_t sound_instance) {
	if (sound_instance > MAX_SOUND_INSTANCES) return;
	mcugdx_mutex_lock(&audio_lock);
	mcugdx_sound_instance_t *instance = &sound_instances[sound_instance];
	if (!instance->sound) {
		mcugdx_mutex_unlock(&audio_lock);
		return;
	}

	// Free the renderer before clearing the instance
	if (instance->renderer) {
		instance->renderer->free(instance->sound, instance->renderer);
		instance->renderer = NULL;
	}

	instance->sound = NULL;
	mcugdx_mutex_unlock(&audio_lock);
}

bool mcugdx_sound_is_playing(mcugdx_sound_id_t sound_instance) {
	if (sound_instance > MAX_SOUND_INSTANCES) return false;
	mcugdx_mutex_lock(&audio_lock);
	bool is_playing = sound_instances[sound_instance].sound != NULL;
	mcugdx_mutex_unlock(&audio_lock);
	return is_playing;
}

static void calculate_pan_gains(uint8_t pan, int32_t *gain_left, int32_t *gain_right) {
	float normalized_pan = (pan - 127) / 128.0f;
	*gain_left = (uint8_t) (255 * (1.0f - normalized_pan) / 2);
	*gain_right = (uint8_t) (255 * (1.0f + normalized_pan) / 2);
}

void mcugdx_audio_mix(int32_t *frames, uint32_t num_frames, mcugdx_audio_channels_t channels) {
	memset(frames, 0, num_frames * channels * sizeof(int32_t));

	mcugdx_mutex_lock(&audio_lock);

	uint32_t active_sources = 0;
	for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
		if (sound_instances[i].sound != NULL) {
			active_sources++;
		}
	}

	if (active_sources == 0) {
		mcugdx_mutex_unlock(&audio_lock);
		return;
	}

	// Mix all active sound instances
	for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
		mcugdx_sound_instance_t *instance = &sound_instances[i];
		if (!instance->sound || !instance->renderer) continue;

		// Calculate pan gains
		int32_t pan_left_gain, pan_right_gain;
		calculate_pan_gains(instance->pan, &pan_left_gain, &pan_right_gain);

		// Calculate final gain (volume)
		int32_t final_gain = instance->volume;

		uint32_t frames_remaining = num_frames;
		uint32_t buffer_offset = 0;

		while (frames_remaining > 0) {
			uint32_t frames_rendered = instance->renderer->render(
					instance->sound,
					instance->renderer,
					frames + (buffer_offset * channels),
					frames_remaining,
					channels,
					pan_left_gain,
					pan_right_gain,
					final_gain);

			if (frames_rendered == 0) {
				if (instance->mode == MCUGDX_LOOP) {
					instance->renderer->reset(instance->sound, instance->renderer);
					continue;
				} else {
					// End of non-looping sound, clean up
					instance->renderer->free(instance->sound, instance->renderer);
					instance->renderer = NULL;
					instance->sound = NULL;
					break;
				}
			}

			frames_remaining -= frames_rendered;
			buffer_offset += frames_rendered;
		}
	}

	mcugdx_mutex_unlock(&audio_lock);

	int32_t max_amplitude = 0;

	// First pass: find max amplitude (removed master volume application)
	for (uint32_t i = 0; i < num_frames * channels; i++) {
		int32_t abs_sample = frames[i] < 0 ? -frames[i] : frames[i];
		if (abs_sample > max_amplitude) max_amplitude = abs_sample;
	}

	// Calculate scaling factor if necessary, with headroom
	float scale = 1.0f;
	if (max_amplitude > INT16_MAX) {
		// Only scale if we're significantly over the limit
		if (max_amplitude > INT16_MAX * 1.05f) {
			scale = (float) INT16_MAX / max_amplitude;
		}
	}

	// Second pass: apply master volume and soft clipping
	int16_t *output = (int16_t *) frames;
	for (uint32_t i = 0; i < num_frames * channels; i++) {
		// Apply master volume only once
		int32_t sample = (frames[i] * master_volume) >> 8;
		sample = (int32_t) (sample * scale);

		// Soft clipping instead of hard clipping
		if (sample > INT16_MAX) {
			float excess = (sample - INT16_MAX) / (float) INT16_MAX;
			sample = INT16_MAX - (int32_t) (INT16_MAX * (1.0f - expf(-excess)));
		} else if (sample < INT16_MIN) {
			float excess = (INT16_MIN - sample) / (float) INT16_MAX;
			sample = INT16_MIN + (int32_t) (INT16_MAX * (1.0f - expf(-excess)));
		}

		output[i] = (int16_t) sample;
	}
}

void mcugdx_audio_set_master_volume(uint8_t volume) {
	master_volume = volume;
}

uint8_t mcugdx_audio_get_master_volume(void) {
	return master_volume;
}
