
#include "mcugdx.h"
#include "doomgeneric/doomgeneric.h"
#include "doomgeneric/w_file.h"
#include "doomgeneric/i_sound.h"
#include "doomgeneric/m_misc.h"
#include "doomgeneric/w_wad.h"
#include "doomgeneric/z_zone.h"
#include "doomgeneric/deh_str.h"
#include "doomgeneric/doomtype.h"
#include "doomgeneric/doomkeys.h"

#define TAG "DOOM"
#define DEBOUNCE_TIME 25

// Big & Little blue fucker
#if 1
mcugdx_display_config_t display_config = {
		.driver = MCUGDX_ST7789,
		.native_width = 240,
		.native_height = 320,
		.mosi = 3,
		.sck = 4,
		.dc = 2,
		.cs = 1,
		.reset = 5};
#else
// ILI9341 2,8" 240x320
mcugdx_display_config_t display_config = {
		.driver = MCUGDX_ILI9341,
		.native_width = 240,
		.native_height = 320,
		.mosi = 3,
		.sck = 4,
		.dc = 2,
		.cs = 1,
		.reset = 5};
#endif

typedef struct {
	wad_file_t wad;
	mcugdx_file_handle_t handle;
} rofs_wad_file_t;

extern wad_file_class_t rofs_wad_file;

static wad_file_t *doom_rofs_open_file(char *path) {
	if (!mcugdx_rofs.exists(path)) return NULL;

	rofs_wad_file_t *result;
	result = mcugdx_mem_alloc(sizeof(rofs_wad_file_t), MCUGDX_MEM_EXTERNAL);
	result->handle = mcugdx_rofs.open(path);
	result->wad.file_class = &rofs_wad_file;
	result->wad.mapped = NULL;
	result->wad.length = mcugdx_rofs.length(result->handle);
	return &result->wad;
}

static void doom_rofs_close_file(wad_file_t *wad) {
	rofs_wad_file_t *rofs_wad;
	rofs_wad = (rofs_wad_file_t *) wad;
	mcugdx_mem_free(rofs_wad);
}

size_t doom_rofs_read(wad_file_t *wad, unsigned int offset,
					  void *buffer, size_t buffer_len) {
	rofs_wad_file_t *rofs_wad = (rofs_wad_file_t *) wad;
	return mcugdx_rofs.read(rofs_wad->handle, offset, buffer, buffer_len);
}

wad_file_class_t rofs_wad_file = {
		doom_rofs_open_file,
		doom_rofs_close_file,
		doom_rofs_read,
};

boolean doom_init_sound(boolean _use_sfx_prefix) {
	// no-op
	return true;
}

void doom_shut_down_sound(void) {
	// no-op
}

void get_sfx_lump_num(sfxinfo_t *sfx, char *buf, size_t buf_len) {

	if (sfx->link != NULL) {
		sfx = sfx->link;
	}
	M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
}

int doom_get_sfx_lump_num(sfxinfo_t *sfx) {
	char namebuf[9];
	get_sfx_lump_num(sfx, namebuf, sizeof(namebuf));
	return W_GetNumForName(namebuf);
}

void doom_update_sound(void) {
	// no-op
}

void doom_update_sound_params(int handle, int vol, int sep) {
	mcugdx_sound_set_volume(handle, vol);
}

int doom_start_sound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
	if (sfxinfo->driver_data) {
		mcugdx_log(TAG, "sep: %li", sep);
		return mcugdx_sound_play((mcugdx_sound_t *) sfxinfo->driver_data, vol, sep, MCUGDX_SINGLE_SHOT);
	} else {
		return -1;
	}
}

void doom_stop_sound(int handle) {
	mcugdx_sound_stop(handle);
}

boolean doom_sound_is_playing(int handle) {
	return mcugdx_sound_is_playing(handle);
}

void doom_precache_sounds(sfxinfo_t *sounds, int num_sounds) {
	for (int i = 0; i < num_sounds; i++) {
		char namebuf[9];
		sfxinfo_t *sfxinfo = &sounds[i];
		get_sfx_lump_num(sfxinfo, namebuf, sizeof(namebuf));
		sfxinfo->lumpnum = W_CheckNumForName(namebuf);
		if (sounds[i].lumpnum == -1) {
			continue;
		}
		unsigned int lumpnum = sfxinfo->lumpnum;
		byte *data = W_CacheLumpNum(lumpnum, PU_STATIC);
		unsigned int lumplen = W_LumpLength(lumpnum);

		if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
			W_ReleaseLumpNum(lumpnum);
			continue;
		}

		int samplerate = (data[3] << 8) | data[2];
		unsigned int length = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];

		mcugdx_log("DOOM_AUDIO", "Caching sound: %s, sample rate: %li, length: %li", namebuf, samplerate, length);

		if (length > lumplen - 8 || length <= 48) {
			W_ReleaseLumpNum(lumpnum);
			continue;
		}

		data += 16;
		length -= 32;


		int16_t *frames = mcugdx_mem_alloc(length * sizeof(int16_t), MCUGDX_MEM_EXTERNAL);
		for (int j = 0; j < length; j++) {
			float sample = data[j] / 127.5 - 1;
			frames[j] = (int16_t) (sample * 32767.0f);
		}
		mcugdx_sound_t *sound = mcugdx_sound_load_raw(frames, length, MCUGDX_MONO, samplerate, MCUGDX_MEM_EXTERNAL);

		W_ReleaseLumpNum(lumpnum);

		sfxinfo->driver_data = sound;
	}
}

static snddevice_t doom_sound_devices[] = {
		SNDDEVICE_SB,
		SNDDEVICE_PAS,
		SNDDEVICE_GUS,
		SNDDEVICE_WAVEBLASTER,
		SNDDEVICE_SOUNDCANVAS,
		SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
		doom_sound_devices,
		arrlen(doom_sound_devices),
		doom_init_sound,
		doom_shut_down_sound,
		doom_get_sfx_lump_num,
		doom_update_sound,
		doom_update_sound_params,
		doom_start_sound,
		doom_stop_sound,
		doom_sound_is_playing,
		doom_precache_sounds,
};

void DG_Init() {
	DG_ScreenBuffer = (pixel_t *) (mcugdx_display_frame_buffer() + mcugdx_display_width() * 20);
}

void DG_SetWindowTitle(const char *title) {
}

int frames = 0;
double last_frame_time = -1;
void DG_DrawFrame(void) {
	mcugdx_display_show();

	frames++;
	if (frames > 60 * 5) {
		frames = 0;
		if (last_frame_time != -1) {
			double frame_time = (mcugdx_time() - last_frame_time) * 1000;
			mcugdx_log(TAG, "Frame time: %f ms, %f fps", frame_time, 1000 / frame_time);
		}
	}
	last_frame_time = mcugdx_time();
}

uint32_t DG_GetTicksMs() {
	uint32_t ticks = (uint32_t) (mcugdx_time() * 1000);
	return ticks;
}

void DG_SleepMs(uint32_t ms) {
	mcugdx_sleep(ms);
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
	mcugdx_button_event_t event;
	if (mcugdx_button_get_event(&event)) {
		*pressed = event.type == MCUGDX_BUTTON_PRESSED ? -1 : 0;
		switch(event.keycode) {
			case MCUGDX_KEY_ESCAPE:
				*doomKey = KEY_ESCAPE;
				break;
			case MCUGDX_KEY_ENTER:
				*doomKey = KEY_ENTER;
				break;
			case MCUGDX_KEY_A:
				*doomKey = KEY_LEFTARROW;
				break;
			case MCUGDX_KEY_S:
				*doomKey = KEY_DOWNARROW;
				break;
			case MCUGDX_KEY_D:
				*doomKey = KEY_RIGHTARROW;
				break;
			case MCUGDX_KEY_W:
				*doomKey = KEY_UPARROW;
				break;
			case MCUGDX_KEY_K:
				*doomKey = KEY_FIRE;
				break;
			case MCUGDX_KEY_L:
				*doomKey = KEY_USE;
				break;
			default:
				return 0;
		}
		return -1;
	} else {
		return 0;
	}
}
int mcugdx_main() {
	mcugdx_init();
	mcugdx_display_init(&display_config);
	mcugdx_display_set_orientation(MCUGDX_LANDSCAPE);
	mcugdx_rofs_init();
	mcugdx_audio_init(&(mcugdx_audio_config_t){
			.channels = 2,
			.sample_rate = 11025,
			.bclk = 47,
			.ws = 21,
			.dout = 38});
    mcugdx_button_create(10, DEBOUNCE_TIME, MCUGDX_KEY_K);
    mcugdx_button_create(9, DEBOUNCE_TIME, MCUGDX_KEY_L);
    mcugdx_button_create(8, DEBOUNCE_TIME, MCUGDX_KEY_ESCAPE);
    mcugdx_button_create(7, DEBOUNCE_TIME, MCUGDX_KEY_ENTER);
    mcugdx_button_create(6, DEBOUNCE_TIME, MCUGDX_KEY_D);
    mcugdx_button_create(12, DEBOUNCE_TIME, MCUGDX_KEY_S);
    mcugdx_button_create(13, DEBOUNCE_TIME, MCUGDX_KEY_A);
    mcugdx_button_create(14, DEBOUNCE_TIME, MCUGDX_KEY_W);

	char *args[] = {"doomgeneric", "-iwad", "Doom1.WAD", "-mmap"};
	doomgeneric_Create(4, args);
	mcugdx_log(TAG, "Game created");
	while (true) {
		doomgeneric_Tick();
	}

	return 0;
}