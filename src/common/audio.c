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

#ifdef _WIN32
	#define strcasecmp _stricmp
#endif

#define TAG "mcugdx_audio"
#define MAX_SOUND_INSTANCES 32

typedef struct {
    bool (*init)(mcugdx_file_handle_t file, mcugdx_file_system_t *fs,
                uint32_t *sample_rate, uint32_t *channels, uint32_t *total_frames,
                void **decoder_state);

    uint32_t (*decode_frames)(void *decoder_state, int32_t *output, uint32_t num_frames,
                             mcugdx_audio_channels_t out_channels, int32_t pan_left_gain,
                             int32_t pan_right_gain, int32_t final_gain);

    void (*reset)(void *decoder_state);

    void (*free)(void *decoder_state);
} mcugdx_audio_decoder_t;

typedef struct mcugdx_sound_internal_t {
	mcugdx_sound_t sound;
	const mcugdx_audio_decoder_t *decoder;
	mcugdx_file_system_t *fs;
	const char *path;
} mcugdx_sound_internal_t;

typedef struct mcugdx_audio_renderer_t mcugdx_audio_renderer_t;

typedef struct {
    mcugdx_file_handle_t file;
    mcugdx_file_system_t *fs;
    qoa_desc qoa;

    // Buffer for encoded QOA frame data
    uint8_t *encoded_buffer;
    uint32_t encoded_buffer_size;

    // Buffer for decoded PCM samples
    int16_t *decoded_buffer;
    uint32_t decoded_buffer_samples;  // Total samples in buffer
    uint32_t decoded_buffer_pos;      // Current position in buffer
} qoa_decoder_state_t;

typedef struct {
    mcugdx_file_handle_t file;
    mcugdx_file_system_t *fs;
    helix_mp3_t mp3;
    helix_mp3_io_t io;

    // Buffer for decoded PCM samples
    int16_t *decoded_buffer;
    uint32_t decoded_buffer_samples;  // Total samples in buffer
    uint32_t decoded_buffer_pos;      // Current position in buffer
    uint32_t channels;                // Cache the channel count
} mp3_decoder_state_t;

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

static bool qoa_init(mcugdx_file_handle_t file, mcugdx_file_system_t *fs,
                    uint32_t *sample_rate, uint32_t *channels, uint32_t *total_frames,
                    void **decoder_state) {
    qoa_decoder_state_t *state = mcugdx_mem_alloc(sizeof(qoa_decoder_state_t), MCUGDX_MEM_EXTERNAL);
    if (!state) return false;

    state->file = file;
    state->fs = fs;

    // Read file header and first frame header (16 bytes total)
    uint8_t header[16];
    if (fs->read(file, header, 16) != 16) {
        mcugdx_mem_free(state);
        return false;
    }

    // Decode the QOA header
    qoa_desc temp_qoa;
    if (!qoa_decode_header(header, 16, &temp_qoa)) {
        mcugdx_mem_free(state);
        return false;
    }

    // Copy the decoded information to our state
    state->qoa.samples = temp_qoa.samples;
    state->qoa.channels = temp_qoa.channels;
    state->qoa.samplerate = temp_qoa.samplerate;

    // Allocate encoded frame buffer
    state->encoded_buffer_size = qoa_max_frame_size(&state->qoa);
    state->encoded_buffer = mcugdx_mem_alloc(state->encoded_buffer_size, MCUGDX_MEM_EXTERNAL);
    if (!state->encoded_buffer) {
        mcugdx_mem_free(state);
        return false;
    }

    // Allocate decoded PCM buffer (one full frame of samples)
    state->decoded_buffer = mcugdx_mem_alloc(QOA_FRAME_LEN * state->qoa.channels * sizeof(int16_t), MCUGDX_MEM_EXTERNAL);
    if (!state->decoded_buffer) {
        mcugdx_mem_free(state->encoded_buffer);
        mcugdx_mem_free(state);
        return false;
    }

    state->decoded_buffer_samples = 0;
    state->decoded_buffer_pos = 0;

    // Set output parameters
    *sample_rate = state->qoa.samplerate;
    *channels = state->qoa.channels;
    *total_frames = state->qoa.samples;
    *decoder_state = state;

	// Reset file position to first frame
	fs->seek(file, 8);

    return true;
}

static uint32_t qoa_decode_frames(void *decoder_state, int32_t *output, uint32_t num_frames,
                                 mcugdx_audio_channels_t out_channels, int32_t pan_left_gain,
                                 int32_t pan_right_gain, int32_t final_gain) {
    qoa_decoder_state_t *state = (qoa_decoder_state_t *)decoder_state;
    uint32_t frames_decoded = 0;
    uint32_t channels = state->qoa.channels;

    while (frames_decoded < num_frames) {
        // If we've used all decoded samples, decode another frame
        if (state->decoded_buffer_pos >= state->decoded_buffer_samples) {
            uint32_t bytes_read = state->fs->read(state->file, state->encoded_buffer, state->encoded_buffer_size);
            if (bytes_read == 0) break;

            uint32_t frame_samples;
            if (!qoa_decode_frame(state->encoded_buffer, bytes_read, &state->qoa,
                                state->decoded_buffer, &frame_samples)) {
                break;
            }

            state->decoded_buffer_samples = frame_samples;
            state->decoded_buffer_pos = 0;
        }

        // Mix samples from decoded buffer to output
        uint32_t samples_available = state->decoded_buffer_samples - state->decoded_buffer_pos;
        uint32_t frames_to_copy = num_frames - frames_decoded;
        if (frames_to_copy > samples_available) {
            frames_to_copy = samples_available;
        }

        int16_t *src = state->decoded_buffer + (state->decoded_buffer_pos * channels);
        int32_t *dst = output + (frames_decoded * out_channels);

        for (uint32_t i = 0; i < frames_to_copy; i++) {
            int32_t left_sample = src[i * channels];
            int32_t right_sample = channels == 1 ? left_sample : src[i * channels + 1];
            dst = mix_frames(dst, out_channels, left_sample, right_sample,
                           pan_left_gain, pan_right_gain, final_gain);
        }

        state->decoded_buffer_pos += frames_to_copy;
        frames_decoded += frames_to_copy;
    }

    return frames_decoded;
}

static void qoa_reset(void *decoder_state) {
    qoa_decoder_state_t *state = (qoa_decoder_state_t *)decoder_state;
    state->fs->seek(state->file, 8); // Seek past file header
    state->decoded_buffer_samples = 0;
    state->decoded_buffer_pos = 0;
}

static void qoa_free(void *decoder_state) {
    qoa_decoder_state_t *state = (qoa_decoder_state_t *)decoder_state;
    if (state) {
        if (state->encoded_buffer) {
            mcugdx_mem_free(state->encoded_buffer);
        }
        if (state->decoded_buffer) {
            mcugdx_mem_free(state->decoded_buffer);
        }
        if (state->file) {
            state->fs->close(state->file);
        }
        mcugdx_mem_free(state);
    }
}

// File-based seek function compatible with helix_mp3_io_t
static int mp3_file_seek(void* ctx, int offset) {
    mp3_decoder_state_t *state = (mp3_decoder_state_t*)ctx;
    return state->fs->seek(state->file, offset) ? 0 : -1;
}

// File-based read function compatible with helix_mp3_io_t
static size_t mp3_file_read(void* ctx, void* buffer, size_t size) {
    mp3_decoder_state_t *state = (mp3_decoder_state_t*)ctx;
    return state->fs->read(state->file, buffer, size);
}

static bool mp3_init(mcugdx_file_handle_t file, mcugdx_file_system_t *fs,
                    uint32_t *sample_rate, uint32_t *channels, uint32_t *total_frames,
                    void **decoder_state) {
    mp3_decoder_state_t *state = mcugdx_mem_alloc(sizeof(mp3_decoder_state_t), MCUGDX_MEM_EXTERNAL);
    if (!state) return false;

    state->file = file;
    state->fs = fs;

    // Setup IO functions
    state->io.seek = mp3_file_seek;
    state->io.read = mp3_file_read;
    state->io.user_data = state;

    // Initialize decoder
    if (helix_mp3_init(&state->mp3, &state->io) != 0) {
        mcugdx_mem_free(state);
        return false;
    }

    // Allocate decoded PCM buffer (one full frame of samples)
    state->channels = helix_mp3_get_channels(&state->mp3);
    state->decoded_buffer = mcugdx_mem_alloc(
        HELIX_MP3_MAX_SAMPLES_PER_FRAME * state->channels * sizeof(int16_t),
        MCUGDX_MEM_EXTERNAL
    );
    if (!state->decoded_buffer) {
        helix_mp3_deinit(&state->mp3);
        mcugdx_mem_free(state);
        return false;
    }

    state->decoded_buffer_samples = 0;
    state->decoded_buffer_pos = 0;

    // Set output parameters
    *sample_rate = helix_mp3_get_sample_rate(&state->mp3);
    *channels = state->channels;
    *total_frames = 0;  // MP3 doesn't provide total frames easily
    *decoder_state = state;

    return true;
}

static uint32_t mp3_decode_frames(void *decoder_state, int32_t *output, uint32_t num_frames,
                                 mcugdx_audio_channels_t out_channels, int32_t pan_left_gain,
                                 int32_t pan_right_gain, int32_t final_gain) {
    mp3_decoder_state_t *state = (mp3_decoder_state_t *)decoder_state;
    uint32_t frames_decoded = 0;

    while (frames_decoded < num_frames) {
        // If we've used all decoded samples, decode another frame
        if (state->decoded_buffer_pos >= state->decoded_buffer_samples) {
            size_t frame_samples = helix_mp3_read_pcm_frames_s16(
                &state->mp3,
                state->decoded_buffer,
                HELIX_MP3_MAX_SAMPLES_PER_FRAME
            );

            if (frame_samples == 0) break;

            state->decoded_buffer_samples = frame_samples;
            state->decoded_buffer_pos = 0;
        }

        // Mix samples from decoded buffer to output
        uint32_t samples_available = state->decoded_buffer_samples - state->decoded_buffer_pos;
        uint32_t frames_to_copy = num_frames - frames_decoded;
        if (frames_to_copy > samples_available) {
            frames_to_copy = samples_available;
        }

        int16_t *src = state->decoded_buffer + (state->decoded_buffer_pos * state->channels);
        int32_t *dst = output + (frames_decoded * out_channels);

        for (uint32_t i = 0; i < frames_to_copy; i++) {
            int32_t left_sample = src[i * state->channels];
            int32_t right_sample = state->channels == 1 ? left_sample : src[i * state->channels + 1];
            dst = mix_frames(dst, out_channels, left_sample, right_sample,
                           pan_left_gain, pan_right_gain, final_gain);
        }

        state->decoded_buffer_pos += frames_to_copy;
        frames_decoded += frames_to_copy;
    }

    return frames_decoded;
}

static void mp3_reset(void *decoder_state) {
    mp3_decoder_state_t *state = (mp3_decoder_state_t *)decoder_state;

    // Reinitialize the decoder
    helix_mp3_deinit(&state->mp3);
    state->fs->seek(state->file, 0);
    helix_mp3_init(&state->mp3, &state->io);

    state->decoded_buffer_samples = 0;
    state->decoded_buffer_pos = 0;
}

static void mp3_free(void *decoder_state) {
    mp3_decoder_state_t *state = (mp3_decoder_state_t *)decoder_state;
    if (state) {
        helix_mp3_deinit(&state->mp3);
        if (state->decoded_buffer) {
            mcugdx_mem_free(state->decoded_buffer);
        }
        if (state->file) {
            state->fs->close(state->file);
        }
        mcugdx_mem_free(state);
    }
}

struct mcugdx_audio_renderer_t {
	void *renderer_data;
	uint32_t (*render)(mcugdx_audio_renderer_t *renderer, int32_t *output, uint32_t num_frames, mcugdx_audio_channels_t channels, int32_t pan_left_gain, int32_t pan_right_gain, int32_t final_gain);
	void (*reset)(mcugdx_audio_renderer_t *renderer);
	void (*free)(mcugdx_audio_renderer_t *renderer);
};

static const mcugdx_audio_decoder_t qoa_decoder = {
    .init = qoa_init,
    .decode_frames = qoa_decode_frames,
    .reset = qoa_reset,
    .free = qoa_free
};

static const mcugdx_audio_decoder_t mp3_decoder = {
    .init = mp3_init,
    .decode_frames = mp3_decode_frames,
    .reset = mp3_reset,
    .free = mp3_free
};

typedef struct {
    mcugdx_sound_internal_t *sound;
    void *decoder_state;              // Format-specific decoder state
    uint8_t volume;
    uint8_t pan;
    mcugdx_playback_mode_t mode;
    uint32_t id;
} mcugdx_sound_instance_t;

static mcugdx_sound_instance_t sound_instances[MAX_SOUND_INSTANCES] = {0};
static uint32_t next_id = 0;
static uint8_t master_volume = 255;
extern mcugdx_mutex_t audio_lock;

mcugdx_sound_t *mcugdx_sound_load(const char *path, mcugdx_file_system_t *fs,
								  mcugdx_sound_type_t sound_type, mcugdx_memory_type_t mem_type) {
	if (!path || !fs) {
		mcugdx_loge(TAG, "Invalid parameters");
		return NULL;
	}

	mcugdx_sound_internal_t *internal = mcugdx_mem_alloc(sizeof(mcugdx_sound_internal_t), mem_type);
	if (!internal) {
		mcugdx_loge(TAG, "Failed to allocate sound internal structure");
		return NULL;
	}

	// Determine format and assign decoder interface
	const char *ext = strrchr(path, '.');
	if (strcasecmp(ext, ".qoa") == 0) {
		internal->decoder = &qoa_decoder;
	} else if (strcasecmp(ext, ".mp3") == 0) {
		internal->decoder = &mp3_decoder;
	} else {
		mcugdx_mem_free(internal);
		return NULL;
	}

	// Create temporary decoder state to get sound properties
	mcugdx_file_handle_t file = fs->open(path);
	if (!file) {
		mcugdx_mem_free(internal);
		return NULL;
	}

	void *temp_decoder_state;
	if (!internal->decoder->init(file, fs, &internal->sound.sample_rate,
							   &internal->sound.channels, &internal->sound.num_frames,
							   &temp_decoder_state)) {
		fs->close(file);
		mcugdx_mem_free(internal);
		return NULL;
	}

	// Store data needed for future decoder creation
	internal->fs = fs;
	internal->path = mcugdx_mem_strdup(path, mem_type);
	internal->sound.type = sound_type;

	// Clean up temporary decoder state
	internal->decoder->free(temp_decoder_state);

	return &internal->sound;
}

void mcugdx_sound_unload(mcugdx_sound_t *sound) {
	if (!sound) return;

	mcugdx_sound_internal_t *internal = (mcugdx_sound_internal_t *)sound;

	// Stop any playing instances of this sound
	mcugdx_mutex_lock(&audio_lock);
	for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
		if (sound_instances[i].sound == internal) {
			if (sound_instances[i].decoder_state) {
				internal->decoder->free(sound_instances[i].decoder_state);
			}
			memset(&sound_instances[i], 0, sizeof(mcugdx_sound_instance_t));
		}
	}
	mcugdx_mutex_unlock(&audio_lock);

	// Free the path string
	if (internal->path) {
		mcugdx_mem_free((void *)internal->path);
	}

	// Free the internal structure itself
	mcugdx_mem_free(internal);
}

double mcugdx_sound_duration(mcugdx_sound_t *sound) {
	mcugdx_sound_internal_t *internal = (mcugdx_sound_internal_t *) sound;
	// FIXME
	return 0;
}

mcugdx_sound_id_t mcugdx_sound_play(mcugdx_sound_t *sound, uint8_t volume, uint8_t pan, mcugdx_playback_mode_t mode) {
	mcugdx_mutex_lock(&audio_lock);

	// Find free slot
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

	// If we're reusing a slot, clean up its existing decoder
	if (!free_slot && instance->decoder_state) {
		instance->sound->decoder->free(instance->decoder_state);
	}

	mcugdx_sound_internal_t *internal = (mcugdx_sound_internal_t *)sound;

	// Create new file handle and decoder state for this instance
	mcugdx_file_handle_t file = internal->fs->open(internal->path);
	if (!file) {
		mcugdx_mutex_unlock(&audio_lock);
		return -1;
	}

	void *decoder_state;
	uint32_t dummy_rate, dummy_channels, dummy_frames;
	if (!internal->decoder->init(file, internal->fs, &dummy_rate, &dummy_channels,
								&dummy_frames, &decoder_state)) {
		internal->fs->close(file);
		mcugdx_mutex_unlock(&audio_lock);
		return -1;
	}

	instance->sound = internal;
	instance->decoder_state = decoder_state;
	instance->volume = volume;
	instance->pan = pan;
	instance->mode = mode;
	instance->id = ++next_id;

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
	if (sound_instance >= MAX_SOUND_INSTANCES) return;

	mcugdx_mutex_lock(&audio_lock);
	mcugdx_sound_instance_t *instance = &sound_instances[sound_instance];

	if (instance->sound && instance->decoder_state) {
		instance->sound->decoder->free(instance->decoder_state);
		instance->decoder_state = NULL;
		instance->sound = NULL;
	}
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

	double start = mcugdx_time();
	for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
		mcugdx_sound_instance_t *instance = &sound_instances[i];
		if (!instance->sound) continue;

		int32_t pan_left_gain, pan_right_gain;
		calculate_pan_gains(instance->pan, &pan_left_gain, &pan_right_gain);
		int32_t final_gain = instance->volume;

		uint32_t frames_remaining = num_frames;
		uint32_t buffer_offset = 0;

		while (frames_remaining > 0) {
			uint32_t frames_decoded = instance->sound->decoder->decode_frames(
				instance->decoder_state,
				frames + (buffer_offset * channels),
				frames_remaining,
				channels,
				pan_left_gain,
				pan_right_gain,
				final_gain
			);

			if (frames_decoded == 0) {
				if (instance->mode == MCUGDX_LOOP) {
					instance->sound->decoder->reset(instance->decoder_state);
					continue;
				} else {
					// Clean up instance
					instance->sound->decoder->free(instance->decoder_state);
					instance->decoder_state = NULL;
					instance->sound = NULL;
					break;
				}
			}

			frames_remaining -= frames_decoded;
			buffer_offset += frames_decoded;
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
