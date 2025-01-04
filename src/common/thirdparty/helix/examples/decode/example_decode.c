#include <helix_mp3.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

#define SAMPLES_PER_FRAME 2
#define PCM_BUFFER_SIZE_SAMPLES (1024 * 32)
#define PCM_BUFFER_SIZE_FRAMES (PCM_BUFFER_SIZE_SAMPLES / SAMPLES_PER_FRAME)

int main(int argc, char *argv[])
{
    if (argc != 3) {
        printf("Usage: %s infile.mp3 outfile.raw\n", argv[0]);
        return -EINVAL;
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];

    helix_mp3_t mp3;
    int16_t pcm_buffer[PCM_BUFFER_SIZE_SAMPLES];
    int err;
    FILE *out_fd;

    /* Initialize decoder */
    err = helix_mp3_init_file(&mp3, input_path);
    if (err) {
        printf("Failed to init decoder for file '%s', error: %d\n", input_path, err);
        return err;
    }

    do {
        /* Open output file */
        out_fd = fopen(output_path, "wb");
        if (out_fd == NULL) {
            printf("Failed to open output file '%s'\n", output_path);
            err = -EIO;
            break;
        }

        printf("Decoding '%s' to '%s'...\n", input_path, output_path);

        /* Decode the whole file */
        while (1) {
            const size_t frames_read = helix_mp3_read_pcm_frames_s16(&mp3, pcm_buffer, PCM_BUFFER_SIZE_FRAMES);
            if (frames_read == 0) {
                printf("Reached EOF!\n");
                break;
            }
            const size_t frames_written = fwrite(pcm_buffer, sizeof(*pcm_buffer) * SAMPLES_PER_FRAME, frames_read, out_fd);
            if (frames_written != frames_read) {
                printf("Failed to write decoded frames to '%s'', expected %zu frames, written %zu frames!\n", output_path, frames_read, frames_written);
                err = -EIO;
                break;
            }
        }

        const size_t frame_count = helix_mp3_get_pcm_frames_decoded(&mp3);
        const uint32_t sample_rate = helix_mp3_get_sample_rate(&mp3);
        const uint32_t bitrate = helix_mp3_get_bitrate(&mp3);
        printf("Done! Decoded %zu frames, last frame sample rate: %" PRIu32 "Hz, bitrate: %" PRIu32 "kbps\n", frame_count, sample_rate, bitrate / 1000);

    } while (0);
    
    /* Cleanup */
    if (out_fd != NULL) {
        fclose(out_fd);
    }
    helix_mp3_deinit(&mp3);

    return err;
}
