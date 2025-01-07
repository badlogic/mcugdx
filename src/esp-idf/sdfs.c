#include "sdfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static sdmmc_card_t *card;
static const char mount_point[] = "/sdcard";

typedef struct {
    FILE* fp;
} sdfs_handle_t;

static bool sdfs_exists(const char *path) {
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s/%s", mount_point, path);
    return access(full_path, F_OK) == 0;
}

static mcugdx_file_handle_t sdfs_open(const char *path) {
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s/%s", mount_point, path);

    sdfs_handle_t* handle = malloc(sizeof(sdfs_handle_t));
    handle->fp = fopen(full_path, "rb");
    return handle;
}

static void sdfs_close(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (h->fp) {
        fclose(h->fp);
    }
    free(h);
}

static uint32_t sdfs_length(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    long current = ftell(h->fp);
    fseek(h->fp, 0, SEEK_END);
    long size = ftell(h->fp);
    fseek(h->fp, current, SEEK_SET);
    return (uint32_t)size;
}

static bool sdfs_seek(mcugdx_file_handle_t handle, uint32_t offset) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    return fseek(h->fp, offset, SEEK_SET) == 0;
}

static uint32_t sdfs_read(mcugdx_file_handle_t handle, uint8_t *buffer, uint32_t buffer_len) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    return fread(buffer, 1, buffer_len, h->fp);
}

static uint8_t *sdfs_read_fully(const char *path, uint32_t *size, mcugdx_memory_type_t mem_type) {
    mcugdx_file_handle_t handle = sdfs_open(path);
    if (!handle) return NULL;

    uint32_t length = sdfs_length(handle);
    uint8_t *buffer = mcugdx_malloc(length, mem_type);
    if (!buffer) {
        sdfs_close(handle);
        return NULL;
    }

    *size = sdfs_read(handle, buffer, length);
    sdfs_close(handle);
    return buffer;
}

static char mount_point[256];

bool mcugdx_sdfs_init(const mcugdx_sdfs_config_t* config) {
    if (!config || !config->mount_path) {
        return false;
    }

    strncpy(mount_point, config->mount_path, sizeof(mount_point) - 1);
    mount_point[sizeof(mount_point) - 1] = '\0';

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
    slot_config.gpio_miso = config->pin_do;
    slot_config.gpio_mosi = config->pin_di;
    slot_config.gpio_sck  = config->pin_clk;
    slot_config.gpio_cs   = config->pin_cs;

    return esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card) == ESP_OK;
}

mcugdx_file_system_t mcugdx_sdfs = {
    .exists = sdfs_exists,
    .open = sdfs_open,
    .close = sdfs_close,
    .length = sdfs_length,
    .seek = sdfs_seek,
    .read = sdfs_read,
    .read_fully = sdfs_read_fully,
    .num_files = NULL,  // Not implemented for SD card
    .file_name = NULL   // Not implemented for SD card
};