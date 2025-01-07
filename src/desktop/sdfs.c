#include "files.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define PATH_SEPARATOR '\\'
#else
#include <dirent.h>
#include <sys/stat.h>
#define PATH_SEPARATOR '/'
#endif

// File handle structure to store state
typedef struct {
    FILE* file;
#ifdef _WIN32
    HANDLE dir_handle;
    WIN32_FIND_DATA find_data;
    bool first_file;
#else
    DIR* dir;
#endif
    char* path;
} sdfs_handle_t;

static char* mount_path = NULL;

// Helper function to build full path
static char* build_full_path(const char* path) {
    size_t mount_len = strlen(mount_path);
    size_t path_len = path ? strlen(path) : 0;
    char* full_path = mcugdx_mem_alloc(mount_len + path_len + 2, MCUGDX_MEM_EXTERNAL);

    strcpy(full_path, mount_path);
    if (path && path[0] != '\0') {
        if (full_path[strlen(full_path) - 1] != PATH_SEPARATOR) {
            char sep[2] = {PATH_SEPARATOR, '\0'};
            strcat(full_path, sep);
        }
        strcat(full_path, path);
    }
    return full_path;
}

static bool sdfs_exists(const char* path) {
    char* full_path = build_full_path(path);
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(full_path);
    bool exists = (attrs != INVALID_FILE_ATTRIBUTES);
#else
    struct stat st;
    bool exists = stat(full_path, &st) == 0;
#endif
    mcugdx_mem_free(full_path);
    return exists;
}

static bool sdfs_is_dir(const char* path) {
    char* full_path = build_full_path(path);
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(full_path);
    bool is_dir = (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    bool is_dir = false;
    if (stat(full_path, &st) == 0) {
        is_dir = S_ISDIR(st.st_mode);
    }
#endif
    mcugdx_mem_free(full_path);
    return is_dir;
}

static mcugdx_file_handle_t sdfs_open(const char* path) {
    char* full_path = build_full_path(path);
    sdfs_handle_t* handle = mcugdx_mem_alloc(sizeof(sdfs_handle_t), MCUGDX_MEM_EXTERNAL);
    handle->file = NULL;
#ifdef _WIN32
    handle->dir_handle = INVALID_HANDLE_VALUE;
    handle->first_file = true;
#else
    handle->dir = NULL;
#endif
    handle->path = mcugdx_mem_alloc(strlen(path) + 1, MCUGDX_MEM_EXTERNAL);
    strcpy(handle->path, path);

    if (sdfs_is_dir(path)) {
#ifdef _WIN32
        char search_path[MAX_PATH];
        snprintf(search_path, sizeof(search_path), "%s\\*", full_path);
        handle->dir_handle = FindFirstFileA(search_path, &handle->find_data);
        if (handle->dir_handle == INVALID_HANDLE_VALUE) {
            mcugdx_mem_free(handle->path);
            mcugdx_mem_free(handle);
            mcugdx_mem_free(full_path);
            return NULL;
        }
#else
        handle->dir = opendir(full_path);
        if (!handle->dir) {
            mcugdx_mem_free(handle->path);
            mcugdx_mem_free(handle);
            mcugdx_mem_free(full_path);
            return NULL;
        }
#endif
    } else {
        handle->file = fopen(full_path, "rb");
        if (!handle->file) {
            mcugdx_mem_free(handle->path);
            mcugdx_mem_free(handle);
            mcugdx_mem_free(full_path);
            return NULL;
        }
    }

    mcugdx_mem_free(full_path);
    return handle;
}

static void sdfs_close(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h) return;

    if (h->file) fclose(h->file);
#ifdef _WIN32
    if (h->dir_handle != INVALID_HANDLE_VALUE) FindClose(h->dir_handle);
#else
    if (h->dir) closedir(h->dir);
#endif
    mcugdx_mem_free(h->path);
    mcugdx_mem_free(h);
}

static mcugdx_file_handle_t sdfs_open_root(void) {
    return sdfs_open("");
}

static void sdfs_file_name(mcugdx_file_handle_t handle, char* buffer, size_t buffer_len) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h || !h->path) return;

    const char* last_slash = strrchr(h->path, '/');
    const char* name = last_slash ? last_slash + 1 : h->path;
    strncpy(buffer, name, buffer_len - 1);
    buffer[buffer_len - 1] = '\0';
}

static void sdfs_full_path(mcugdx_file_handle_t handle, char* buffer, size_t buffer_len) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h || !h->path) return;

    strncpy(buffer, h->path, buffer_len - 1);
    buffer[buffer_len - 1] = '\0';
}

static bool sdfs_is_dir_handle(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    return h && h->dir != NULL;
}

static mcugdx_file_handle_t sdfs_read_dir(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
#ifdef _WIN32
    if (!h || h->dir_handle == INVALID_HANDLE_VALUE) return NULL;

    while (1) {
        if (!h->first_file && !FindNextFileA(h->dir_handle, &h->find_data)) {
            return NULL;
        }
        h->first_file = false;

        if (strcmp(h->find_data.cFileName, ".") == 0 || strcmp(h->find_data.cFileName, "..") == 0) {
            continue;
        }

        char* new_path;
        if (strlen(h->path) == 0) {
            new_path = mcugdx_mem_alloc(strlen(h->find_data.cFileName) + 1, MCUGDX_MEM_EXTERNAL);
            strcpy(new_path, h->find_data.cFileName);
        } else {
            size_t new_path_len = strlen(h->path) + strlen(h->find_data.cFileName) + 2;
            new_path = mcugdx_mem_alloc(new_path_len, MCUGDX_MEM_EXTERNAL);
            snprintf(new_path, new_path_len, "%s\\%s", h->path, h->find_data.cFileName);
        }

        mcugdx_file_handle_t new_handle = sdfs_open(new_path);
        mcugdx_mem_free(new_path);
        if (new_handle) {
            return new_handle;
        }
    }
#else
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
#endif
    return NULL;
}

static uint32_t sdfs_length(mcugdx_file_handle_t handle) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h || !h->file) return 0;

    long current = ftell(h->file);
    fseek(h->file, 0, SEEK_END);
    long size = ftell(h->file);
    fseek(h->file, current, SEEK_SET);
    return (uint32_t)size;
}

static bool sdfs_seek(mcugdx_file_handle_t handle, uint32_t offset) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h || !h->file) return false;
    return fseek(h->file, offset, SEEK_SET) == 0;
}

static uint32_t sdfs_read(mcugdx_file_handle_t handle, uint8_t* buffer, uint32_t buffer_len) {
    sdfs_handle_t* h = (sdfs_handle_t*)handle;
    if (!h || !h->file) return 0;
    return (uint32_t)fread(buffer, 1, buffer_len, h->file);
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
    if (!config || !config->mount_path) return false;

    if (mount_path) {
        mcugdx_mem_free(mount_path);
    }

    mount_path = mcugdx_mem_alloc(strlen(config->mount_path) + 1, MCUGDX_MEM_EXTERNAL);
    strcpy(mount_path, config->mount_path);
    return true;
}

mcugdx_file_system_t mcugdx_sdfs = {
    .exists = sdfs_exists,
    .is_dir = sdfs_is_dir,
    .open = sdfs_open,
    .close = sdfs_close,
    .open_root = sdfs_open_root,
    .file_name = sdfs_file_name,
    .full_path = sdfs_full_path,
    .is_dir_handle = sdfs_is_dir_handle,
    .read_dir = sdfs_read_dir,
    .length = sdfs_length,
    .seek = sdfs_seek,
    .read = sdfs_read,
    .read_fully = sdfs_read_fully
};
