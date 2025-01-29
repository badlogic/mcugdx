#pragma once

#include "mem.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Change handle to be an opaque pointer that implementations can use to store state
typedef void* mcugdx_file_handle_t;

typedef bool (*mcugdx_fs_exists_func_t)(const char *path);
typedef bool (*mcugdx_fs_is_dir_func_t)(const char *path);
typedef mcugdx_file_handle_t (*mcugdx_fs_open_func_t)(const char *path);
typedef void (*mcugdx_fs_close_func_t)(mcugdx_file_handle_t);
typedef mcugdx_file_handle_t (*mcugdx_fs_open_root_func_t)(void);
typedef void (*mcugdx_fs_file_name_func_t)(mcugdx_file_handle_t handle, char *buffer, size_t buffer_len);
typedef void (*mcugdx_fs_full_path_func_t)(mcugdx_file_handle_t handle, char *buffer, size_t buffer_len);
typedef bool (*mcugdx_fs_is_dir_handle_func_t)(mcugdx_file_handle_t handle);
typedef mcugdx_file_handle_t (*mcugdx_fs_read_dir_func_t)(mcugdx_file_handle_t handle);
typedef uint32_t (*mcugdx_fs_length_func_t)(mcugdx_file_handle_t handle);
typedef bool (*mcugdx_fs_seek_func_t)(mcugdx_file_handle_t handle, uint32_t offset);
typedef uint32_t (*mcugdx_fs_read_func_t)(mcugdx_file_handle_t handle, uint8_t *buffer, uint32_t buffer_len);
typedef uint8_t *(*mcugdx_fs_read_fully_func_t)(const char *path, uint32_t *size, mcugdx_memory_type_t mem_type);


typedef struct {
    mcugdx_fs_exists_func_t exists;
    mcugdx_fs_is_dir_func_t is_dir;
    mcugdx_fs_open_func_t open;
    mcugdx_fs_close_func_t close;
    mcugdx_fs_open_root_func_t open_root;
    mcugdx_fs_file_name_func_t file_name;
    mcugdx_fs_full_path_func_t full_path;
    mcugdx_fs_is_dir_handle_func_t is_dir_handle;
    mcugdx_fs_read_dir_func_t read_dir;
    mcugdx_fs_length_func_t length;
    mcugdx_fs_seek_func_t seek;
    mcugdx_fs_read_func_t read;
    mcugdx_fs_read_fully_func_t read_fully;
} mcugdx_file_system_t;

bool mcugdx_rofs_init(void);
extern mcugdx_file_system_t mcugdx_rofs;

typedef struct {
    int pin_clk;
    int pin_cmd;
    int pin_d0;
    int pin_cd;  // New field for card detect pin
    const char* mount_path;  // Used on desktop to specify the directory the file system will work off of
} mcugdx_sdfs_config_t;

bool mcugdx_sdfs_init(const mcugdx_sdfs_config_t* config);
void mcugdx_sdfs_deinit(void);  // New function to tear down SDFS
bool mcugdx_sdfs_is_card_present(void);  // New function to check card presence
extern mcugdx_file_system_t mcugdx_sdfs;

#ifdef __cplusplus
}
#endif
