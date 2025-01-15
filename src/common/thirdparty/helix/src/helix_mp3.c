#include "helix_mp3.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "mem.h"

#define HELIX_MP3_MIN(x, y) (((x) < (y)) ? (x) : (y))
#define ID3V2_FRAME_HEADER_SIZE    10
#define ID3V2_FRAME_OFFSET         0
#define ID3V2_MAGIC_STRING_LENGTH  3
#define ID3V2_MAGIC_STRING         "ID3"

static int helix_mp3_skip_id3v2_tag(helix_mp3_t *mp3)
{
    uint8_t frame_buffer[ID3V2_FRAME_HEADER_SIZE];

    /* Seek to the beginning of the frame and read frame's header */
    if (mp3->io->seek(mp3->io->user_data, ID3V2_FRAME_OFFSET) != 0) {
        return -EIO;
    }
    if (mp3->io->read(mp3->io->user_data, frame_buffer, ID3V2_FRAME_HEADER_SIZE) != ID3V2_FRAME_HEADER_SIZE) {
        return -EIO;
    }

    /* Check magic */
    if (strncmp((const char *)frame_buffer, ID3V2_MAGIC_STRING, ID3V2_MAGIC_STRING_LENGTH) != 0) {
        mp3->io->seek(mp3->io->user_data, ID3V2_FRAME_OFFSET);
        return 0;
    }

    /* The tag size (minus the 10-byte header) is encoded into four bytes,
     * but the most significant bit needs to be masked in each byte.
     * Those frame indices are just copied from the ID3V2 docs. */
    const size_t id3v2_tag_total_size = (((frame_buffer[6] & 0x7F) << 21) | ((frame_buffer[7] & 0x7F) << 14) |
                                        ((frame_buffer[8] & 0x7F) << 7) | ((frame_buffer[9] & 0x7F) << 0)) +
                                        ID3V2_FRAME_HEADER_SIZE;

    /* Skip the tag */
    if (mp3->io->seek(mp3->io->user_data, ID3V2_FRAME_OFFSET + id3v2_tag_total_size) != 0) {
        return -EIO;
    }
    return id3v2_tag_total_size;
}

static size_t helix_mp3_fill_mp3_buffer(helix_mp3_t *mp3)
{
    /* Move remaining data to the beginning of the buffer */
    memmove(&mp3->mp3_buffer[0], mp3->mp3_read_ptr, mp3->mp3_buffer_bytes_left);

    /* Read new data */
    const size_t bytes_to_read = HELIX_MP3_DATA_CHUNK_SIZE - mp3->mp3_buffer_bytes_left;
    const size_t bytes_read = mp3->io->read(mp3->io->user_data, &mp3->mp3_buffer[mp3->mp3_buffer_bytes_left], sizeof(*mp3->mp3_buffer) * bytes_to_read);

    /* Zero-pad to avoid finding false sync word from old data */
    if (bytes_read < bytes_to_read) {
        memset(&mp3->mp3_buffer[mp3->mp3_buffer_bytes_left + bytes_read], 0, bytes_to_read - bytes_read);
    }

    return bytes_read;
}

static size_t helix_mp3_decode_next_frame(helix_mp3_t *mp3)
{
    size_t pcm_samples_read;

    while (1) {
        if (mp3->mp3_buffer_bytes_left < HELIX_MP3_MIN_DATA_CHUNK_SIZE) {
            const size_t bytes_read = helix_mp3_fill_mp3_buffer(mp3);
            mp3->mp3_buffer_bytes_left += bytes_read;
            mp3->mp3_read_ptr = &mp3->mp3_buffer[0];
        }

        const int offset = MP3FindSyncWord(mp3->mp3_read_ptr, mp3->mp3_buffer_bytes_left);
        if (offset < 0) {
            pcm_samples_read = 0;
            break; // Out of data
        }
        mp3->mp3_read_ptr += offset;
        mp3->mp3_buffer_bytes_left -= offset;

        const int err = MP3Decode(mp3->dec, &mp3->mp3_read_ptr, &mp3->mp3_buffer_bytes_left, mp3->pcm_buffer, 0);
        if (err == ERR_MP3_NONE) {
            MP3FrameInfo frame_info;
            MP3GetLastFrameInfo(mp3->dec, &frame_info);

            mp3->current_sample_rate = frame_info.samprate;
            mp3->current_bitrate = frame_info.bitrate;
            mp3->pcm_samples_left = frame_info.outputSamps;
            mp3->current_channels = frame_info.nChans;

            pcm_samples_read = mp3->pcm_samples_left;
            break;
        }
        else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
            continue; // Get more data from file
        }
        else {
            pcm_samples_read = 0;
            break; // Out of data
        }
    }

    return pcm_samples_read;
}

static int helix_mp3_seek(void *user_data, int offset)
{
    return fseek((FILE *)user_data, offset, SEEK_SET);
}

static size_t helix_mp3_read(void *user_data, void *buffer, size_t size)
{
    return fread(buffer, sizeof(uint8_t), size, (FILE *)user_data);
}

static helix_mp3_io_t default_io =
{
    .seek = helix_mp3_seek,
    .read = helix_mp3_read
};

int helix_mp3_init(helix_mp3_t *mp3, const helix_mp3_io_t *io)
{
    if ((mp3 == NULL) || (io == NULL)) {
        return -EINVAL;
    }

    memset(mp3, 0, sizeof(*mp3));
    mp3->io = io;

    int err = 0;
    do {
        mp3->dec = MP3InitDecoder();
        if (mp3->dec == NULL) {
            err = -ENOMEM;
            break;
        }

        mp3->mp3_buffer = mcugdx_mem_alloc(HELIX_MP3_DATA_CHUNK_SIZE, MCUGDX_MEM_EXTERNAL);
        if (mp3->mp3_buffer == NULL) {
            err = -ENOMEM;
            break;
        }
        mp3->pcm_buffer = mcugdx_mem_alloc(HELIX_MP3_MAX_SAMPLES_PER_FRAME * sizeof(*mp3->pcm_buffer), MCUGDX_MEM_EXTERNAL);
        if (mp3->pcm_buffer == NULL) {
            err = -ENOMEM;
            break;
        }

        if (helix_mp3_skip_id3v2_tag(mp3) < 0) {
            err = -EIO;
            break;
        }

        if (helix_mp3_decode_next_frame(mp3) == 0) {
            err = -ENOTSUP;
            break;
        }
    } while (0);

    if (err) {
        mcugdx_mem_free(mp3->pcm_buffer);
        mcugdx_mem_free(mp3->mp3_buffer);
        MP3FreeDecoder(mp3->dec);
    }
    return err;
}


int helix_mp3_init_file(helix_mp3_t *mp3, const char *path)
{
    /* Open input file */
    FILE *fd = fopen(path, "rb");
    if (fd == NULL) {
       return -ENOENT;
    }
    default_io.user_data = fd;

    /* Initialize decoder */
    const int err = helix_mp3_init(mp3, &default_io);
    if (err) {
        fclose(fd);
        return err;
    }
    return 0;
}


int helix_mp3_deinit(helix_mp3_t *mp3)
{
    if (mp3 == NULL) {
        return -EINVAL;
    }

    if (mp3->io->read == default_io.read) {
        fclose((FILE *)mp3->io->user_data);
    }
    mcugdx_mem_free(mp3->pcm_buffer);
    mcugdx_mem_free(mp3->mp3_buffer);
    MP3FreeDecoder(mp3->dec);

    return 0;
}

uint32_t helix_mp3_get_sample_rate(helix_mp3_t *mp3)
{
    if (mp3 == NULL) {
        return 0;
    }
    return mp3->current_sample_rate;
}

uint32_t helix_mp3_get_bitrate(helix_mp3_t *mp3)
{
    if (mp3 == NULL) {
        return 0;
    }
    return mp3->current_bitrate;
}

size_t helix_mp3_get_pcm_frames_decoded(helix_mp3_t *mp3)
{
    if (mp3 == NULL) {
        return 0;
    }
	return mp3->current_pcm_frame;
}

size_t helix_mp3_read_pcm_frames_s16(helix_mp3_t *mp3, int16_t *buffer, size_t frames_to_read)
{
    if ((mp3 == NULL) || (buffer == NULL) || (frames_to_read == 0)) {
        return 0;
    }

    size_t samples_to_read = frames_to_read * mp3->current_channels;
    size_t samples_read = 0;

    while (1) {
        const size_t samples_to_consume = HELIX_MP3_MIN(mp3->pcm_samples_left, samples_to_read);

        memcpy(&buffer[samples_read], mp3->pcm_buffer,
               samples_to_consume * sizeof(*mp3->pcm_buffer));

        mp3->current_pcm_frame += (samples_to_consume / mp3->current_channels);
        mp3->pcm_samples_left -= samples_to_consume;
        samples_read += samples_to_consume;
        samples_to_read -= samples_to_consume;

        /* In-memory PCM buffer is fully used up, decode next frame */
        if (mp3->pcm_samples_left == 0) {
            if (helix_mp3_decode_next_frame(mp3) == 0) {
                break;
            }
        }

        /* Job done */
        if (samples_to_read == 0) {
            break;
        }
    }

    return (samples_read / mp3->current_channels);
}

uint8_t helix_mp3_get_channels(helix_mp3_t *mp3)
{
    if (mp3 == NULL) {
        return 0;
    }
    return mp3->current_channels;
}
