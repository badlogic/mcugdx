#pragma once

#include "mem.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* rofs_file_handle_t;

bool rofs_init(void);

bool rofs_exists(const char *path);

rofs_file_handle_t rofs_open(const char *path);

void rofs_close(rofs_file_handle_t handle);

uint32_t rofs_length(rofs_file_handle_t handle);

bool rofs_seek(rofs_file_handle_t handle, uint32_t offset);

uint32_t rofs_read(rofs_file_handle_t handle, uint8_t *buffer, uint32_t buffer_len);

uint8_t *rofs_read_fully(const char *path, uint32_t *size, mcugdx_memory_type_t mem_type);

int32_t rofs_num_files(void);

const char *rofs_file_name(int32_t index);

bool rofs_is_dir(const char *path);

rofs_file_handle_t rofs_open_root(void);

void rofs_file_name_handle(rofs_file_handle_t handle, char *buffer, size_t buffer_len);

void rofs_full_path(rofs_file_handle_t handle, char *buffer, size_t buffer_len);

bool rofs_is_dir_handle(rofs_file_handle_t handle);

rofs_file_handle_t rofs_read_dir(rofs_file_handle_t handle);

#ifdef __cplusplus
}
#endif
