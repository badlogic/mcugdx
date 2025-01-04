#include <helix_mp3.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

#define SAMPLES_PER_FRAME 2
#define PCM_BUFFER_SIZE_SAMPLES (1024 * 32)
#define PCM_BUFFER_SIZE_FRAMES (PCM_BUFFER_SIZE_SAMPLES / SAMPLES_PER_FRAME)

static int seek_callback(void *user_data, int offset)
{
    int fd = *(int *)user_data;
    return (lseek(fd, offset, SEEK_SET) < 0);
}

static size_t read_callback(void *user_data, void *buffer, size_t size)
{
    int fd = *(int *)user_data;
    return read(fd, buffer, size);
}

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
    int in_fd = -1;
    int out_fd = -1;

    /* Create I/O interface */
    helix_mp3_io_t io = {
        .seek = seek_callback,
        .read = read_callback,
        .user_data = &in_fd
    };

    /* Open an input stream */
    in_fd = open(input_path, O_RDONLY);
    if (in_fd < 0) {
        printf("Failed to open input file '%s', error: %d\n", input_path, errno);
        return -EIO;
    }

    do {
        /* Initialize decoder */
        err = helix_mp3_init(&mp3, &io);
        if (err) {
            printf("Failed to init decoder for file '%s', error: %d\n", input_path, err);
            break;
        }

        /* Open output file */
        out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            printf("Failed to open output file '%s', error: %d\n", output_path, errno);
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
            const size_t bytes_written = write(out_fd, pcm_buffer, sizeof(*pcm_buffer) * SAMPLES_PER_FRAME * frames_read);
            const size_t frames_written = bytes_written / (sizeof(*pcm_buffer) * SAMPLES_PER_FRAME);
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
    close(out_fd);
    close(in_fd);
    helix_mp3_deinit(&mp3);

    return err;
}
