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

static uint32_t next_id = 0;

// Add format-specific decoder struct
typedef struct {
	void (*decode_frame)(const uint8_t* data, uint32_t size, void* decoder_data, int16_t* output, uint32_t* num_frames);
	void (*free_decoder)(void* decoder_data);
	void* decoder_data;
} mcugdx_audio_decoder_t;

typedef struct {
	mcugdx_sound_t base;
	union{
		struct {
			int16_t *frames;
		} preloaded;
		struct {
			mcugdx_file_system_t *fs;
			mcugdx_file_handle_t file;
			mcugdx_audio_decoder_t decoder;
			uint32_t first_frame_pos;
			uint32_t decoding_buffer_size;
			uint32_t frames_size_in_bytes;
		} streamed;
	};
} mcugdx_sound_internal_t;

typedef struct {
	mcugdx_sound_internal_t *sound;
	uint8_t volume;
	uint8_t pan;
	mcugdx_playback_mode_t mode;
	uint32_t position;
	uint32_t id;

	// Only used for streamed sounds
	uint32_t file_offset;
	uint32_t frames_size_in_bytes;
	int16_t *frames;
	uint32_t num_frames;
	uint32_t frames_pos;
} mcugdx_sound_instance_t;

static mcugdx_sound_instance_t sound_instances[MAX_SOUND_INSTANCES] = {0};
uint8_t master_volume = 128;
extern mcugdx_mutex_t audio_lock;
static uint8_t *decoding_buffer = NULL;
static uint32_t decoding_buffer_size = 0;

// QOA-specific decoder implementation
typedef struct {
	qoa_desc qoa;
} qoa_decoder_data_t;

static void qoa_decode_frame_wrapper(const uint8_t* data, uint32_t size, void* decoder_data, int16_t* output, uint32_t* num_frames) {
	qoa_decoder_data_t* qoa_data = (qoa_decoder_data_t*)decoder_data;
	qoa_decode_frame(data, size, &qoa_data->qoa, output, num_frames);
}

static void qoa_free_decoder(void* decoder_data) {
	mcugdx_mem_free(decoder_data);
}

// Update MP3-specific decoder implementation
typedef struct {
	helix_mp3_t helix;
	mcugdx_file_system_t* fs;
	mcugdx_file_handle_t file;
} mp3_decoder_data_t;

static void mp3_decode_frame_wrapper(const uint8_t* data, uint32_t size, void* decoder_data, int16_t* output, uint32_t* num_frames) {
	mp3_decoder_data_t* mp3_data = (mp3_decoder_data_t*)decoder_data;
	*num_frames = helix_mp3_read_pcm_frames_s16(&mp3_data->helix, output, QOA_FRAME_LEN);
	*num_frames *= 2; // Convert frames to samples (always stereo output)
}

static void mp3_free_decoder(void* decoder_data) {
	mp3_decoder_data_t* mp3_data = (mp3_decoder_data_t*)decoder_data;
	helix_mp3_deinit(&mp3_data->helix);
	mp3_data->fs->close(mp3_data->file);
	mcugdx_mem_free(decoder_data);
}

static int mp3_seek_wrapper(void* user_data, int offset) {
	mp3_decoder_data_t* mp3_data = (mp3_decoder_data_t*)user_data;
	return mp3_data->fs->seek(mp3_data->file, offset) ? 0 : -1;
}

static size_t mp3_read_wrapper(void* user_data, void* buffer, size_t bytes_to_read) {
	mp3_decoder_data_t* mp3_data = (mp3_decoder_data_t*)user_data;
	return mp3_data->fs->read(mp3_data->file, buffer, bytes_to_read);
}

// Format-specific decoder interface
typedef struct {
	const char* extension;
	bool (*init_streaming)(const char* path, mcugdx_file_system_t* fs, mcugdx_memory_type_t mem_type,
						  mcugdx_audio_decoder_t* decoder, uint32_t* first_frame_pos,
						  uint32_t* sample_rate, uint32_t* channels, uint32_t* num_samples,
						  uint32_t* buffer_size, uint32_t* frames_size);
	bool (*init_preloaded)(const char* path, mcugdx_file_system_t* fs, mcugdx_memory_type_t mem_type,
						  int16_t** frames, uint32_t* sample_rate, uint32_t* channels, uint32_t* num_samples);
} mcugdx_audio_format_t;

// QOA implementation
static bool qoa_init_streaming(const char* path, mcugdx_file_system_t* fs, mcugdx_memory_type_t mem_type,
							  mcugdx_audio_decoder_t* decoder, uint32_t* first_frame_pos,
							  uint32_t* sample_rate, uint32_t* channels, uint32_t* num_samples,
							  uint32_t* buffer_size, uint32_t* frames_size) {
	mcugdx_file_handle_t file = fs->open(path);
	if (!file) return false;

	unsigned char header[QOA_MIN_FILESIZE];
	if (!fs->read(file, header, QOA_MIN_FILESIZE)) {
		fs->close(file);
		return false;
	}

	qoa_decoder_data_t* qoa_data = mcugdx_mem_alloc(sizeof(qoa_decoder_data_t), mem_type);
	qoa_desc qoa;

	*first_frame_pos = qoa_decode_header(header, QOA_MIN_FILESIZE, &qoa);
	if (!*first_frame_pos) {
		mcugdx_mem_free(qoa_data);
		fs->close(file);
		return false;
	}

	*sample_rate = qoa.samplerate;
	*channels = qoa.channels;
	*num_samples = qoa.samples;
	*buffer_size = qoa_max_frame_size(&qoa);
	*frames_size = qoa.channels * QOA_FRAME_LEN * sizeof(int16_t) * 2;

	qoa_data->qoa = qoa;
	decoder->decode_frame = qoa_decode_frame_wrapper;
	decoder->free_decoder = qoa_free_decoder;
	decoder->decoder_data = qoa_data;

	fs->close(file);
	return true;
}

static bool qoa_init_preloaded(const char* path, mcugdx_file_system_t* fs, mcugdx_memory_type_t mem_type,
							  int16_t** frames, uint32_t* sample_rate, uint32_t* channels, uint32_t* num_samples) {
	uint32_t size;
	uint8_t* raw = fs->read_fully(path, &size, MCUGDX_MEM_EXTERNAL);
	if (!raw) return false;

	qoa_desc qoa;
	if (!qoa_decode_header(raw, size, &qoa)) {
		mcugdx_mem_free(raw);
		return false;
	}

	*frames = qoa_decode(raw, size, &qoa, mem_type);
	*sample_rate = qoa.samplerate;
	*channels = qoa.channels;
	*num_samples = qoa.samples;

	mcugdx_mem_free(raw);
	return *frames != NULL;
}

// Add a helper struct to store both fs and file handle
typedef struct {
	mcugdx_file_system_t* fs;
	mcugdx_file_handle_t file;
} mp3_io_context_t;

// Update the MP3 streaming initialization
static bool mp3_init_streaming(const char* path, mcugdx_file_system_t* fs, mcugdx_memory_type_t mem_type,
							  mcugdx_audio_decoder_t* decoder, uint32_t* first_frame_pos,
							  uint32_t* sample_rate, uint32_t* channels, uint32_t* num_samples,
							  uint32_t* buffer_size, uint32_t* frames_size) {
	mp3_decoder_data_t* mp3_data = mcugdx_mem_alloc(sizeof(mp3_decoder_data_t), mem_type);
	if (!mp3_data) return false;

	mp3_data->fs = fs;
	mp3_data->file = fs->open(path);
	if (!mp3_data->file) {
		mcugdx_mem_free(mp3_data);
		mcugdx_mem_free(mp3_data);
		return false;
	}

	// Setup I/O callbacks for Helix
	helix_mp3_io_t io = {
		.seek = mp3_seek_wrapper,
		.read = mp3_read_wrapper,
		.user_data = mp3_data
	};

	if (helix_mp3_init(&mp3_data->helix, &io) != 0) {
		fs->close(mp3_data->file);
		mcugdx_mem_free(mp3_data);
		mcugdx_mem_free(mp3_data);
		return false;
	}

	*first_frame_pos = 0; // Not needed for MP3
	*sample_rate = helix_mp3_get_sample_rate(&mp3_data->helix);
	*channels = 2; // Helix always outputs stereo
	*num_samples = 0; // We don't know the total number of samples upfront
	*buffer_size = HELIX_MP3_DATA_CHUNK_SIZE;
	*frames_size = QOA_FRAME_LEN * 2 * sizeof(int16_t); // Same buffer size as QOA for consistency

	decoder->decode_frame = mp3_decode_frame_wrapper;
	decoder->free_decoder = mp3_free_decoder;
	decoder->decoder_data = mp3_data;

	return true;
}

// Update the MP3 preloaded initialization similarly
static bool mp3_init_preloaded(const char* path, mcugdx_file_system_t* fs, mcugdx_memory_type_t mem_type,
							 int16_t** frames, uint32_t* sample_rate, uint32_t* channels, uint32_t* num_samples) {
	// Create and initialize IO context
	mp3_decoder_data_t* decoder = mcugdx_mem_alloc(sizeof(mp3_decoder_data_t), MCUGDX_MEM_EXTERNAL);
	if (!decoder) return false;

	decoder->fs = fs;
	decoder->file = fs->open(path);
	if (!decoder->file) {
		mcugdx_mem_free(decoder);
		return false;
	}

	// Setup I/O callbacks for Helix
	helix_mp3_io_t io = {
		.seek = mp3_seek_wrapper,
		.read = mp3_read_wrapper,
		.user_data = decoder
	};

	helix_mp3_t helix;
	if (helix_mp3_init(&helix, &io) != 0) {
		fs->close(decoder->file);
		mcugdx_mem_free(decoder);
		return false;
	}

	// First pass: count total frames
	size_t total_frames = 0;
	int16_t temp_buffer[HELIX_MP3_MAX_SAMPLES_PER_FRAME];
	size_t frames_read;

	do {
		frames_read = helix_mp3_read_pcm_frames_s16(&helix, temp_buffer, HELIX_MP3_MAX_SAMPLES_PER_FRAME/2);
		total_frames += frames_read;
	} while (frames_read > 0);

	// Reset decoder for second pass
	fs->seek(decoder->file, 0);
	helix_mp3_deinit(&helix);
	if (helix_mp3_init(&helix, &io) != 0) {
		fs->close(decoder->file);
		mcugdx_mem_free(decoder);
		return false;
	}

	// Allocate buffer for entire sound
	*frames = mcugdx_mem_alloc(total_frames * 2 * sizeof(int16_t), mem_type);
	if (!*frames) {
		helix_mp3_deinit(&helix);
		fs->close(decoder->file);
		mcugdx_mem_free(decoder);
		return false;
	}

	// Second pass: decode entire file
	size_t frames_decoded = 0;
	while (frames_decoded < total_frames) {
		size_t frames_to_read = total_frames - frames_decoded;
		if (frames_to_read > HELIX_MP3_MAX_SAMPLES_PER_FRAME/2) {
			frames_to_read = HELIX_MP3_MAX_SAMPLES_PER_FRAME/2;
		}

		frames_read = helix_mp3_read_pcm_frames_s16(&helix,
			*frames + (frames_decoded * 2), // *2 because stereo
			frames_to_read);

		if (frames_read == 0) break;
		frames_decoded += frames_read;
	}

	*sample_rate = helix_mp3_get_sample_rate(&helix);
	*channels = 2; // Helix always outputs stereo
	*num_samples = frames_decoded;

	helix_mp3_deinit(&helix);
	fs->close(decoder->file);
	mcugdx_mem_free(decoder);
	return true;
}

static const mcugdx_audio_format_t formats[] = {
	{ ".qoa", qoa_init_streaming, qoa_init_preloaded },
	{ ".mp3", mp3_init_streaming, mp3_init_preloaded }, // Add MP3 support
	{ NULL, NULL, NULL }
};

mcugdx_sound_t* mcugdx_sound_load(const char* path, mcugdx_file_system_t* fs,
								 mcugdx_sound_type_t sound_type, mcugdx_memory_type_t mem_type) {
	const char* ext = strrchr(path, '.');
	if (!ext) {
		mcugdx_loge(TAG, "No file extension found for %s", path);
		return NULL;
	}

	const mcugdx_audio_format_t* format = NULL;
	for (int i = 0; formats[i].extension != NULL; i++) {
		if (strcmp(ext, formats[i].extension) == 0) {
			format = &formats[i];
			break;
		}
	}

	if (!format) {
		mcugdx_loge(TAG, "Unsupported audio format: %s", ext);
		return NULL;
	}

	mcugdx_sound_internal_t* sound = mcugdx_mem_alloc(sizeof(mcugdx_sound_internal_t), mem_type);
	if (!sound) return NULL;

	if (sound_type == MCUGDX_PRELOADED) {
		int16_t* frames;
		uint32_t sample_rate, channels, num_samples;

		if (!format->init_preloaded(path, fs, mem_type, &frames, &sample_rate, &channels, &num_samples)) {
			mcugdx_mem_free(sound);
			return NULL;
		}

		if (sample_rate != mcugdx_audio_get_sample_rate()) {
			mcugdx_loge(TAG, "Sample rate mismatch: %li != %li", sample_rate, mcugdx_audio_get_sample_rate());
			mcugdx_mem_free(frames);
			mcugdx_mem_free(sound);
			return NULL;
		}

		sound->base.type = MCUGDX_PRELOADED;
		sound->base.sample_rate = sample_rate;
		sound->base.channels = channels;
		sound->base.num_frames = num_samples;
		sound->preloaded.frames = frames;
	} else {
		mcugdx_audio_decoder_t decoder;
		uint32_t first_frame_pos, sample_rate, channels, num_samples, buffer_size, frames_size;

		if (!format->init_streaming(path, fs, mem_type, &decoder, &first_frame_pos,
								  &sample_rate, &channels, &num_samples, &buffer_size, &frames_size)) {
			mcugdx_mem_free(sound);
			return NULL;
		}

		if (sample_rate != mcugdx_audio_get_sample_rate()) {
			mcugdx_loge(TAG, "Sample rate mismatch: %li != %li", sample_rate, mcugdx_audio_get_sample_rate());
			decoder.free_decoder(decoder.decoder_data);
			mcugdx_mem_free(sound);
			return NULL;
		}

		sound->base.type = MCUGDX_STREAMED;
		sound->base.sample_rate = sample_rate;
		sound->base.channels = channels;
		sound->base.num_frames = num_samples;
		sound->streamed.fs = fs;
		sound->streamed.file = fs->open(path);
		sound->streamed.decoder = decoder;
		sound->streamed.first_frame_pos = first_frame_pos;
		sound->streamed.decoding_buffer_size = buffer_size;
		sound->streamed.frames_size_in_bytes = frames_size;

		if (decoding_buffer_size < buffer_size) {
			mcugdx_mutex_lock(&audio_lock);
			mcugdx_mem_free(decoding_buffer);
			decoding_buffer = mcugdx_mem_alloc(buffer_size, MCUGDX_MEM_EXTERNAL);
			decoding_buffer_size = buffer_size;
			mcugdx_mutex_unlock(&audio_lock);
		}
	}

	return &sound->base;
}

mcugdx_sound_t *mcugdx_sound_load_raw(int16_t *frames, uint32_t num_frames,
									  mcugdx_audio_channels_t channels,
									  uint32_t sample_rate,
									  mcugdx_memory_type_t mem_type) {
	if (sample_rate != mcugdx_audio_get_sample_rate()) {
		mcugdx_loge(
				TAG, "Sample rate of raw sound %li != audio system sample rate %li",
				sample_rate, mcugdx_audio_get_sample_rate());
		return NULL;
	}

	mcugdx_sound_internal_t *sound = (mcugdx_sound_internal_t *) mcugdx_mem_alloc(
			sizeof(mcugdx_sound_internal_t), mem_type);
	sound->base.type = MCUGDX_PRELOADED;
	sound->base.sample_rate = sample_rate;
	sound->base.channels = channels;
	sound->base.num_frames = num_frames;
	sound->preloaded.frames = frames;

	return &sound->base;
}

void mcugdx_sound_unload(mcugdx_sound_t *sound) {
	mcugdx_mutex_lock(&audio_lock);
	for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
		mcugdx_sound_instance_t *instance = &sound_instances[i];
		if (instance->sound == (mcugdx_sound_internal_t *) sound) {
			if (instance->sound->base.type == MCUGDX_STREAMED) {
				mcugdx_mem_free(instance->frames);
				instance->frames = NULL;
			}
			instance->sound = NULL;
		}
	}
	mcugdx_sound_internal_t *internal = (mcugdx_sound_internal_t *) sound;
	if (sound->type == MCUGDX_STREAMED) {
		internal->streamed.fs->close(internal->streamed.file);
		if (internal->streamed.decoder.free_decoder) {
			internal->streamed.decoder.free_decoder(internal->streamed.decoder.decoder_data);
		}
	}
	mcugdx_mem_free(sound);
	mcugdx_mutex_unlock(&audio_lock);
}

double mcugdx_sound_duration(mcugdx_sound_t *sound) {
	mcugdx_sound_internal_t *internal = (mcugdx_sound_internal_t *) sound;
	return internal->base.num_frames / (double) internal->base.sample_rate;
}

static uint32_t stream_audio_frame(mcugdx_sound_instance_t *instance) {
	if (instance->sound->base.type == MCUGDX_PRELOADED) return 0;
	mcugdx_sound_internal_t *sound = (mcugdx_sound_internal_t *)instance->sound;

	if (!instance->frames || instance->frames_size_in_bytes != sound->streamed.frames_size_in_bytes) {
		mcugdx_mem_free(instance->frames);
		instance->frames = mcugdx_mem_alloc(sound->streamed.frames_size_in_bytes, MCUGDX_MEM_EXTERNAL);
		instance->frames_size_in_bytes = sound->streamed.frames_size_in_bytes;
	}

	sound->streamed.fs->seek(sound->streamed.file, instance->file_offset + sound->streamed.first_frame_pos);
	uint32_t read_bytes = sound->streamed.fs->read(
		sound->streamed.file,
		decoding_buffer,
		sound->streamed.decoding_buffer_size
	);

	instance->file_offset += read_bytes;
	uint32_t num_samples = 0;

	sound->streamed.decoder.decode_frame(
		decoding_buffer,
		read_bytes,
		sound->streamed.decoder.decoder_data,
		instance->frames,
		&num_samples
	);

	instance->frames_pos = 0;
	instance->num_frames = num_samples / sound->base.channels;
	return instance->num_frames;
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

	instance->sound = (mcugdx_sound_internal_t *) sound;
	instance->volume = volume;
	instance->pan = pan;
	instance->mode = mode;
	instance->position = 0;
	instance->id = ++next_id;
	instance->file_offset = 0;
	instance->frames_size_in_bytes = 0;
	instance->frames = NULL;
	instance->num_frames = 0;
	instance->frames_pos = 0;
	stream_audio_frame(instance);
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
	if (instance->sound->base.type == MCUGDX_STREAMED) {
		mcugdx_mem_free(instance->frames);
		instance->frames = NULL;
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
	*gain_left = (uint8_t)(255 * (1.0f - normalized_pan) / 2);
	*gain_right = (uint8_t)(255 * (1.0f + normalized_pan) / 2);
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

	for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
		mcugdx_sound_instance_t *instance = &sound_instances[i];
		mcugdx_sound_internal_t *sound = instance->sound;
		if (instance->sound == NULL) {
			continue;
		}

		uint32_t frames_to_mix = num_frames;
		uint32_t source_position = instance->position;
		int32_t instance_volume = instance->volume;
		int32_t final_gain = instance_volume;
		int32_t pan_left_gain, pan_right_gain;
		calculate_pan_gains(instance->pan, &pan_left_gain, &pan_right_gain);
		if (channels == MCUGDX_MONO) {
			pan_left_gain = 255;
			pan_right_gain = 255;
		}

		bool is_streamed = sound->base.type == MCUGDX_STREAMED;
		uint32_t sound_channels = sound->base.channels;
		while (frames_to_mix > 0) {
			uint32_t frames_left_in_sound = sound->base.num_frames - source_position;
			uint32_t frames_to_process = (frames_to_mix < frames_left_in_sound) ? frames_to_mix : frames_left_in_sound;

			for (uint32_t frame = 0; frame < frames_to_process; frame++) {
				int32_t left_sample, right_sample;

				if (!is_streamed) {
					if (sound_channels == 1) {
						int32_t mono_sample = sound->preloaded.frames[source_position];
						left_sample = right_sample = mono_sample;
					} else {
						left_sample = sound->preloaded.frames[source_position * 2];
						right_sample = sound->preloaded.frames[source_position * 2 + 1];
					}
				} else {
					if (instance->num_frames - instance->frames_pos == 0) {
						if (!stream_audio_frame(instance)) {
							mcugdx_loge(TAG, "Could not decode remaining frames of sound. This should never happen");
						}
					}

					if (sound_channels == 1) {
						int32_t mono_sample = instance->frames[instance->frames_pos++];
						left_sample = right_sample = mono_sample;
					} else {
						left_sample = instance->frames[instance->frames_pos * 2];
						right_sample = instance->frames[instance->frames_pos * 2 + 1];
					}
				}

				left_sample = ((left_sample * pan_left_gain) >> 8) * final_gain >> 8;
				right_sample = ((right_sample * pan_right_gain) >> 8) * final_gain >> 8;

				if (channels == MCUGDX_MONO) {
					frames[frame] += (left_sample + right_sample) >> 1;
				} else {
					frames[frame * 2] += left_sample;
					frames[frame * 2 + 1] += right_sample;
				}

				source_position++;
			}

			frames_to_mix -= frames_to_process;

			if (source_position >= instance->sound->base.num_frames) {
				if (instance->mode == MCUGDX_LOOP) {
					source_position = 0;
					instance->file_offset = 0;
				} else {
					if (instance->sound->base.type == MCUGDX_STREAMED) {
						mcugdx_mem_free(instance->frames);
						instance->frames = NULL;
					}
					instance->sound = NULL;
					break;
				}
			}
		}

		instance->position = source_position;
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
			scale = (float)INT16_MAX / max_amplitude;
		}
	}

	// Second pass: apply master volume and soft clipping
	int16_t *output = (int16_t *)frames;
	for (uint32_t i = 0; i < num_frames * channels; i++) {
		// Apply master volume only once
		int32_t sample = (frames[i] * master_volume) >> 8;
		sample = (int32_t)(sample * scale);

		// Soft clipping instead of hard clipping
		if (sample > INT16_MAX) {
			float excess = (sample - INT16_MAX) / (float)INT16_MAX;
			sample = INT16_MAX - (int32_t)(INT16_MAX * (1.0f - expf(-excess)));
		} else if (sample < INT16_MIN) {
			float excess = (INT16_MIN - sample) / (float)INT16_MAX;
			sample = INT16_MIN + (int32_t)(INT16_MAX * (1.0f - expf(-excess)));
		}

		output[i] = (int16_t)sample;
	}
}

void mcugdx_audio_set_master_volume(uint8_t volume) {
	master_volume = volume;
}

uint8_t mcugdx_audio_get_master_volume(void) {
	return master_volume;
}
