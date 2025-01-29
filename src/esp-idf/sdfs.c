#include "files.h"
#include "log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include "gpio.h"

#define MOUNT_POINT "/sdcard"

// Add at top of file with other globals
static int cd_pin = -1;  // Store CD pin for polling

// File handle structure to maintain state
typedef struct {
    FILE* fp;
    DIR* dir;
    char* path;  // Dynamically allocated path
} sdfs_handle_t;

mcugdx_file_system_t mcugdx_sdfs;

static bool sdfs_exists(const char* path) {
    char* full_path = mcugdx_mem_alloc(strlen(path) + strlen(MOUNT_POINT) + 2, MCUGDX_MEM_EXTERNAL);
    snprintf(full_path, strlen(path) + strlen(MOUNT_POINT) + 2, MOUNT_POINT "/%s", path);

    struct stat st;
    bool exists = stat(full_path, &st) == 0;

    mcugdx_mem_free(full_path);
    return exists;
}

static bool sdfs_is_dir(const char* path) {
    char* full_path = mcugdx_mem_alloc(strlen(path) + strlen(MOUNT_POINT) + 2, MCUGDX_MEM_EXTERNAL);
    snprintf(full_path, strlen(path) + strlen(MOUNT_POINT) + 2, MOUNT_POINT "/%s", path);

    struct stat st;
    bool is_dir = false;
    if (stat(full_path, &st) == 0) {
        is_dir = S_ISDIR(st.st_mode);
    }

    mcugdx_mem_free(full_path);
    return is_dir;
}

static mcugdx_file_handle_t sdfs_open(const char* path) {
    char* full_path = mcugdx_mem_alloc(strlen(path) + strlen(MOUNT_POINT) + 2, MCUGDX_MEM_EXTERNAL);
    snprintf(full_path, strlen(path) + strlen(MOUNT_POINT) + 2, MOUNT_POINT "/%s", path);

    sdfs_handle_t* handle = mcugdx_mem_alloc(sizeof(sdfs_handle_t), MCUGDX_MEM_EXTERNAL);
    if (!handle) {
        mcugdx_mem_free(full_path);
        return NULL;
    }

    handle->fp = fopen(full_path, "rb");
    handle->dir = NULL;
    handle->path = mcugdx_mem_alloc(strlen(path) + 1, MCUGDX_MEM_EXTERNAL);
    strcpy(handle->path, path);

    mcugdx_mem_free(full_path);

    if (!handle->fp) {
        mcugdx_mem_free(handle->path);
        mcugdx_mem_free(handle);
        return NULL;
    }
    return handle;
}

static void sdfs_close(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h) return;

    if (h->fp) fclose(h->fp);
    if (h->dir) closedir(h->dir);
    mcugdx_mem_free(h->path);
    mcugdx_mem_free(h);
}

static mcugdx_file_handle_t sdfs_open_root(void) {
    sdfs_handle_t* handle = mcugdx_mem_alloc(sizeof(sdfs_handle_t), MCUGDX_MEM_EXTERNAL);
    if (!handle) return NULL;

    handle->fp = NULL;
    handle->dir = opendir(MOUNT_POINT);
    handle->path = mcugdx_mem_alloc(1, MCUGDX_MEM_EXTERNAL); // Allocate space for empty string
    if (!handle->path) {
        if (handle->dir) closedir(handle->dir);
        mcugdx_mem_free(handle);
        return NULL;
    }
    handle->path[0] = '\0'; // Initialize as empty string

    if (!handle->dir) {
        mcugdx_mem_free(handle->path);
        mcugdx_mem_free(handle);
        return NULL;
    }
    return handle;
}

static void sdfs_file_name(mcugdx_file_handle_t handle, char* buffer, size_t buffer_len) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h) return;
    const char* last_slash = strrchr(h->path, '/');
    const char* name = last_slash ? last_slash + 1 : h->path;
    strncpy(buffer, name, buffer_len - 1);
    buffer[buffer_len - 1] = '\0';
}

static void sdfs_full_path(mcugdx_file_handle_t handle, char* buffer, size_t buffer_len) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h) return;
    snprintf(buffer, buffer_len, "%s", h->path);
}

static bool sdfs_is_dir_handle(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    return h && h->dir != NULL;
}

static mcugdx_file_handle_t sdfs_read_dir(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h || !h->dir) return NULL;

    struct dirent* entry;
    while ((entry = readdir(h->dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Create a new handle for the found entry
        char* new_path;
        if (strlen(h->path) == 0) {
            new_path = mcugdx_mem_alloc(strlen(entry->d_name) + 1, MCUGDX_MEM_EXTERNAL);
            strcpy(new_path, entry->d_name);
        } else {
            size_t new_path_len = strlen(h->path) + strlen(entry->d_name) + 2;
            new_path = mcugdx_mem_alloc(new_path_len, MCUGDX_MEM_EXTERNAL);
            snprintf(new_path, new_path_len, "%s/%s", h->path, entry->d_name);
        }

        mcugdx_file_handle_t new_handle = sdfs_open(new_path);
        mcugdx_mem_free(new_path);
        if (new_handle) {
            return new_handle;
        }
    }
    return NULL;
}

static uint32_t sdfs_length(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h || !h->fp) return 0;

    long current = ftell(h->fp);
    fseek(h->fp, 0, SEEK_END);
    long size = ftell(h->fp);
    fseek(h->fp, current, SEEK_SET);
    return (uint32_t)size;
}

static bool sdfs_seek(mcugdx_file_handle_t handle, uint32_t offset) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h || !h->fp) return false;
    return fseek(h->fp, offset, SEEK_SET) == 0;
}

static uint32_t sdfs_read(mcugdx_file_handle_t handle, uint8_t* buffer, uint32_t buffer_len) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h || !h->fp) return 0;
    return (uint32_t)fread(buffer, 1, buffer_len, h->fp);
}

static uint8_t* sdfs_read_fully(const char* path, uint32_t* size, mcugdx_memory_type_t mem_type) {
    mcugdx_file_handle_t handle = sdfs_open(path);
    if (!handle) return NULL;

    uint32_t file_size = sdfs_length(handle);
    uint8_t* buffer = mcugdx_mem_alloc(file_size, mem_type);

    if (buffer) {
        *size = sdfs_read(handle, buffer, file_size);
        if (*size != file_size) {
            mcugdx_mem_free(buffer);
            buffer = NULL;
        }
    }

    sdfs_close(handle);
    return buffer;
}

bool mcugdx_sdfs_init(const mcugdx_sdfs_config_t* config) {
    // Always set up the CD pin, regardless of card presence
    cd_pin = config->pin_cd;
    mcugdx_gpio_pin_mode(cd_pin, MCUGDX_DIGITAL_INPUT, MCUGDX_PULL_NONE);

    // If no card is present, return early but pin is still configured
    if (!mcugdx_sdfs_is_card_present()) {
        return false;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    // slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot_config.clk = (gpio_num_t)config->pin_clk;
    slot_config.cmd = (gpio_num_t)config->pin_cmd;  // CMD = DI (MOSI)
    slot_config.d0 = (gpio_num_t)config->pin_d0;   // D0 = DO (MISO)

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t* card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        return false;
    }

    // Initialize the file system interface
    mcugdx_sdfs.exists = sdfs_exists;
    mcugdx_sdfs.is_dir = sdfs_is_dir;
    mcugdx_sdfs.open = sdfs_open;
    mcugdx_sdfs.close = sdfs_close;
    mcugdx_sdfs.open_root = sdfs_open_root;
    mcugdx_sdfs.file_name = sdfs_file_name;
    mcugdx_sdfs.full_path = sdfs_full_path;
    mcugdx_sdfs.is_dir_handle = sdfs_is_dir_handle;
    mcugdx_sdfs.read_dir = sdfs_read_dir;
    mcugdx_sdfs.length = sdfs_length;
    mcugdx_sdfs.seek = sdfs_seek;
    mcugdx_sdfs.read = sdfs_read;
    mcugdx_sdfs.read_fully = sdfs_read_fully;

    return true;
}

void mcugdx_sdfs_deinit(void) {
    if (esp_vfs_fat_sdmmc_unmount() != ESP_OK) {
        mcugdx_log("sdfs", "Failed to unmount SD card");
    }
}

bool mcugdx_sdfs_is_card_present(void) {
    if (cd_pin < 0) return false;
    return mcugdx_gpio_digital_in(cd_pin) == 1;  // HIGH means card is present
}