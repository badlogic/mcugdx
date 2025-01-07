#include "rofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "mem.h"

#define TAG "mcugdx_rofs"

#ifdef ESP_PLATFORM
#include "esp_partition.h"
#else
#include <limits.h>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

typedef struct {
	char *name;
	uint32_t offset;
	uint32_t size;
} rofs_file_t;

typedef struct {
	uint32_t num_files;
	rofs_file_t *files;
	uint32_t data_offset;
} rofs_file_system_t;

rofs_file_system_t fs = {0};

typedef struct rofs_file_handle_struct {
	uint32_t index;
	uint32_t current_offset;
} rofs_file_handle_struct_t;

#ifdef ESP_PLATFORM
const esp_partition_t *partition;

void read_line(const void *partition, size_t *offset, char *buffer, size_t max_len) {
	char ch;
	size_t i = 0;

	while (i < max_len - 1) {
		esp_partition_read(partition, (*offset)++, &ch, 1);
		if (ch == '\n') {
			break;
		}
		buffer[i++] = ch;
	}

	buffer[i] = '\0';
}

uint8_t *rofs_read_fully(const char *path, uint32_t *size, mcugdx_memory_type_t mem_type) {
	for (int i = 0; i < fs.num_files; i++) {
		if (strcmp(fs.files[i].name, path) == 0) {
			uint32_t file_offset = fs.files[i].offset;
			uint32_t file_size = fs.files[i].size;
			uint8_t *copy = (uint8_t *) mcugdx_mem_alloc(file_size, mem_type);
			if (!copy) {
				mcugdx_loge(TAG, "Failed to allocate memory for file %s\n", path);
				return NULL;
			}
			if (esp_partition_read(partition, file_offset, copy, file_size) != ESP_OK) {
				free(copy);
				mcugdx_loge(TAG, "Failed to read file %s\n", path);
				return NULL;
			}

			*size = file_size;
			return copy;
		}
	}

	mcugdx_loge(TAG, "File not found: %s\n", path);
	*size = 0;
	return NULL;
}

#else
const uint8_t *partition;

void get_executable_dir(char *path, size_t size) {
#if defined(_WIN32)
	GetModuleFileName(NULL, path, size);
	for (int i = strlen(path) - 1; i >= 0; i--) {
		if (path[i] == '\\') {
			path[i] = '\0';
			break;
		}
	}
#elif defined(__linux__)
	ssize_t len = readlink("/proc/self/exe", path, size - 1);
	if (len != -1) {
		path[len] = '\0';
		for (int i = strlen(path) - 1; i >= 0; i--) {
			if (path[i] == '/') {
				path[i] = '\0';
				break;
			}
		}
	}
#elif defined(__APPLE__)
	uint32_t len = size;
	_NSGetExecutablePath(path, &len);
	for (int i = strlen(path) - 1; i >= 0; i--) {
		if (path[i] == '/') {
			path[i] = '\0';
			break;
		}
	}
#endif
}

#ifdef _WIN32
#define PATH_SEPARATOR '\\'
#else
#define PATH_SEPARATOR '/'
#endif

uint8_t *read_partition_file(void) {
    char executable_dir[1024];
    get_executable_dir(executable_dir, sizeof(executable_dir));

    char rofs_bin_path[1060];
    snprintf(rofs_bin_path, sizeof(rofs_bin_path), "%s%c%s", executable_dir, PATH_SEPARATOR, "rofs.bin");

    FILE *file = fopen(rofs_bin_path, "rb");
    if (!file) {
        snprintf(rofs_bin_path, sizeof(rofs_bin_path), "%s%c..%crofs.bin", executable_dir, PATH_SEPARATOR, PATH_SEPARATOR);
        file = fopen(rofs_bin_path, "rb");
        if (!file) {
            fprintf(stderr, "Failed to open file: %s\n", rofs_bin_path);
            return NULL;
        }
    }

	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);

	uint8_t *partition = malloc(file_size);
	if (!partition) {
		mcugdx_loge(TAG, "Failed to allocate memory.\n");
		fclose(file);
		return NULL;
	}

	fread(partition, 1, file_size, file);
	fclose(file);

	return partition;
}

void read_line(const uint8_t *partition, size_t *offset, char *buffer, size_t max_len) {
	char ch;
	size_t i = 0;

	while (i < max_len - 1) {
		ch = partition[(*offset)++];
		if (ch == '\n') {
			break;
		}
		buffer[i++] = ch;
	}

	buffer[i] = '\0';
}

uint8_t *rofs_read_fully(const char *path, uint32_t *size, mcugdx_memory_type_t mem_type) {
	(void) mem_type;

	for (uint32_t i = 0; i < fs.num_files; i++) {
		if (strcmp(fs.files[i].name, path) == 0) {
			uint32_t file_offset = fs.files[i].offset;
			uint32_t file_size = fs.files[i].size;
			const uint8_t *data = partition + file_offset;

			uint8_t *copy = (uint8_t *) mcugdx_mem_alloc(file_size, mem_type);
			if (!copy) {
				mcugdx_loge(TAG, "Failed to allocate memory for file\n");
				return NULL;
			}

			memcpy(copy, data, file_size);
			*size = file_size;
			return copy;
		}
	}

	mcugdx_loge(TAG, "File not found: %s\n", path);
	return NULL;
}
#endif

bool rofs_init(void) {
#ifdef ESP_PLATFORM
	partition = esp_partition_find_first(
			ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "rofs");
	if (!partition) {
		mcugdx_loge(TAG, "Failed to find rofs partition\n");
		return false;
	}
#else
	partition = read_partition_file();
	if (!partition) {
		mcugdx_loge(TAG, "Failed to load rofs.bin\n");
		return false;
	}
#endif

	size_t offset = 0;
	char line[256];
	read_line(partition, &offset, line, sizeof(line));
	int num_files = atoi(line);
	fs.num_files = num_files;
	fs.files = mcugdx_mem_alloc(num_files * sizeof(rofs_file_t), MCUGDX_MEM_EXTERNAL);

	for (int i = 0; i < num_files; i++) {
		read_line(partition, &offset, line, sizeof(line));
		size_t file_name_len = strnlen(line, sizeof(line));
		fs.files[i].name = mcugdx_mem_alloc(file_name_len + 1, MCUGDX_MEM_EXTERNAL);
		memcpy(fs.files[i].name, line, file_name_len + 1);

		read_line(partition, &offset, line, sizeof(line));
		fs.files[i].offset = atoi(line);

		read_line(partition, &offset, line, sizeof(line));
		fs.files[i].size = atoi(line);
	}

	for (int i = 0; i < num_files; i++) {
		fs.files[i].offset += offset;
	}

	return true;
}

int32_t rofs_num_files(void) {
	return fs.num_files;
}

const char *rofs_file_name(int32_t index) {
	return fs.files[index].name;
}

bool rofs_exists(const char *path) {
	for (uint32_t i = 0; i < fs.num_files; i++) {
		if (strcmp(fs.files[i].name, path) == 0) {
			return true;
		}
	}
	return false;
}

rofs_file_handle_t rofs_open(const char *path) {
	for (uint32_t i = 0; i < fs.num_files; i++) {
		if (strcmp(fs.files[i].name, path) == 0) {
			rofs_file_handle_struct_t* handle = mcugdx_mem_alloc(sizeof(rofs_file_handle_struct_t), MCUGDX_MEM_EXTERNAL);
			if (!handle) return NULL;
			handle->index = i;
			handle->current_offset = 0;
			return handle;
		}
	}
	return NULL;
}

uint32_t rofs_length(rofs_file_handle_t handle) {
	rofs_file_handle_struct_t* handle_struct = (rofs_file_handle_struct_t*) handle;
	return fs.files[handle_struct->index].size;
}

bool rofs_seek(rofs_file_handle_t handle, uint32_t offset) {
	rofs_file_handle_struct_t* handle_struct = (rofs_file_handle_struct_t*) handle;
	if (offset >= fs.files[handle_struct->index].size) {
		return false;
	}
	handle_struct->current_offset = offset;
	return true;
}

uint32_t rofs_read(rofs_file_handle_t handle, uint8_t *buffer, uint32_t buffer_len) {
	rofs_file_handle_struct_t* handle_struct = (rofs_file_handle_struct_t*) handle;
	uint32_t file_size = fs.files[handle_struct->index].size;
	uint32_t file_start_offset = fs.files[handle_struct->index].offset;

	if (handle_struct->current_offset >= file_size) {
		mcugdx_loge(TAG, "Current offset %u past end of file %s",
			handle_struct->current_offset, fs.files[handle_struct->index].name);
		return 0;
	}

	uint32_t bytes_to_read = file_size - handle_struct->current_offset;
	if (bytes_to_read > buffer_len) {
		bytes_to_read = buffer_len;
	}

#ifdef ESP_PLATFORM
	esp_err_t result = esp_partition_read(partition,
										file_start_offset + handle_struct->current_offset,
										buffer,
										bytes_to_read);
	if (result != ESP_OK) {
		mcugdx_loge(TAG, "Failed to read file %s at offset %u\n",
			fs.files[handle_struct->index].name, handle_struct->current_offset);
		return 0;
	}
#else
	const uint8_t *data = partition + file_start_offset + handle_struct->current_offset;
	memcpy(buffer, data, bytes_to_read);
#endif

	handle_struct->current_offset += bytes_to_read;
	return bytes_to_read;
}

void rofs_close(rofs_file_handle_t handle) {
	mcugdx_mem_free(handle);
}

bool rofs_is_dir(const char *path) {
    for (uint32_t i = 0; i < fs.num_files; i++) {
        if (strcmp(fs.files[i].name, path) == 0) {
            return fs.files[i].size == (uint32_t)-1;
        }
    }
    return false;
}

rofs_file_handle_t rofs_open_root(void) {
    rofs_file_handle_struct_t* handle = mcugdx_mem_alloc(sizeof(rofs_file_handle_struct_t), MCUGDX_MEM_EXTERNAL);
    if (!handle) return NULL;
    handle->index = (uint32_t)-1;
    handle->current_offset = 0;
    return handle;
}

void rofs_file_name_handle(rofs_file_handle_t handle, char *buffer, size_t buffer_len) {
    rofs_file_handle_struct_t* handle_struct = (rofs_file_handle_struct_t*)handle;
    if (handle_struct->index == (uint32_t)-1) {
        strncpy(buffer, "", buffer_len);
        return;
    }
    const char* full_path = fs.files[handle_struct->index].name;

    // Find last separator
    const char* last_sep = strrchr(full_path, '/');
    const char* name = last_sep ? last_sep + 1 : full_path;

    strncpy(buffer, name, buffer_len);
    buffer[buffer_len - 1] = '\0';
}

void rofs_full_path(rofs_file_handle_t handle, char *buffer, size_t buffer_len) {
    rofs_file_handle_struct_t* handle_struct = (rofs_file_handle_struct_t*)handle;
    if (handle_struct->index == (uint32_t)-1) {
        strncpy(buffer, "", buffer_len);
        return;
    }
    strncpy(buffer, fs.files[handle_struct->index].name, buffer_len);
    buffer[buffer_len - 1] = '\0';
}

bool rofs_is_dir_handle(rofs_file_handle_t handle) {
    rofs_file_handle_struct_t* handle_struct = (rofs_file_handle_struct_t*)handle;
    if (handle_struct->index == (uint32_t)-1) {
        return true;
    }
    return fs.files[handle_struct->index].size == (uint32_t)-1;
}

static rofs_file_handle_t create_file_handle(uint32_t index) {
    rofs_file_handle_struct_t* new_handle = mcugdx_mem_alloc(sizeof(rofs_file_handle_struct_t), MCUGDX_MEM_EXTERNAL);
    if (!new_handle) return NULL;
    new_handle->index = index;
    new_handle->current_offset = 0;
    return new_handle;
}

rofs_file_handle_t rofs_read_dir(rofs_file_handle_t handle) {
    rofs_file_handle_struct_t* handle_struct = (rofs_file_handle_struct_t*)handle;

    // Get the current directory's path
    const char* current_dir = (handle_struct->index == (uint32_t)-1) ?
                            "" : fs.files[handle_struct->index].name;
    size_t current_dir_len = strlen(current_dir);

    // Special handling for root directory
    bool is_root = (handle_struct->index == (uint32_t)-1);

    // Look for the next file/directory that's a direct child of the current directory
    while (handle_struct->current_offset < fs.num_files) {
        uint32_t next_idx = handle_struct->current_offset++;
        const char* next_path = fs.files[next_idx].name;

        if (is_root) {
            // For root directory, only include entries with no '/'
            if (strchr(next_path, '/') == NULL) {
                return create_file_handle(next_idx);
            }
        } else {
            // Original logic for non-root directories
            if (strcmp(next_path, current_dir) == 0) {
                continue;
            }

            if (strncmp(next_path, current_dir, current_dir_len) == 0) {
                const char* remaining = next_path + current_dir_len;
                if (*remaining == '/') {
                    remaining++;
                    if (strchr(remaining, '/') == NULL) {
                        return create_file_handle(next_idx);
                    }
                }
            }
        }
    }

    return NULL;
}
