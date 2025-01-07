#include "mcugdx.h"
#include <string.h>

#define TAG "files"

void print_filesystem_entry(mcugdx_file_system_t fs, mcugdx_file_handle_t handle, int depth) {
	char name[256];
	char indent[32] = {0};

	// Create indent based on depth
	for (int i = 0; i < depth; i++) {
		strcat(indent, "  ");
	}

	// Get the file/directory name
	fs.file_name(handle, name, sizeof(name));

	// Print the entry with appropriate indent
	if (fs.is_dir_handle(handle)) {
		mcugdx_log(TAG, "%s[DIR] %s", indent, name);
	} else {
		uint32_t size = fs.length(handle);
		mcugdx_log(TAG, "%s[FILE] %s (%lu bytes)", indent, name, (unsigned long) size);
	}
}

void traverse_directory(mcugdx_file_system_t fs, mcugdx_file_handle_t dir_handle, int depth) {
	mcugdx_file_handle_t entry;

	// Read each entry in the current directory
	while ((entry = fs.read_dir(dir_handle)) != NULL) {
		print_filesystem_entry(fs, entry, depth);

		// If it's a directory, recursively traverse it
		if (fs.is_dir_handle(entry)) {
			traverse_directory(fs, entry, depth + 1);
		}

		fs.close(entry);
	}
}

extern "C" int mcugdx_main() {
	mcugdx_init();
	mcugdx_log(TAG, "Starting filesystem traversal:");

	mcugdx_mem_print();

	// Initialize both filesystems
	if (!mcugdx_rofs_init()) {
		mcugdx_log(TAG, "Failed to initialize ROFS");
		return -1;
	}

   mcugdx_sdfs_config_t sdfs_config = {
			.mount_path = "../data"};
	if (!mcugdx_sdfs_init(&sdfs_config)) {
		mcugdx_log(TAG, "Failed to initialize SDFS");
		return -1;
	}

	// Traverse ROFS root
	/*{
		mcugdx_log(TAG, "ROFS contents:");
		mcugdx_file_handle_t root = mcugdx_rofs.open_root();
		if (root != NULL) {
			traverse_directory(mcugdx_rofs, root, 0);
			mcugdx_rofs.close(root);
		} else {
			mcugdx_log(TAG, "Failed to open root directory");
			return -1;
		}
	}*/

	// Traverse SDFS data directory
	{
		mcugdx_log(TAG, "\nSDFS data/ directory contents:");
		mcugdx_file_handle_t root = mcugdx_sdfs.open_root();
		if (root != NULL) {
			traverse_directory(mcugdx_sdfs, root, 0);
			mcugdx_sdfs.close(root);
		} else {
			mcugdx_log(TAG, "Failed to open data directory");
		}
	}

	mcugdx_mem_print();

	return 0;
}