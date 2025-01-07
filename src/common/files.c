#include "files.h"
#include "rofs.h"

bool mcugdx_rofs_init(void) {
	return rofs_init();
}

mcugdx_file_system_t mcugdx_rofs = {
	.exists = rofs_exists,
	.is_dir = rofs_is_dir,
	.open = rofs_open,
	.close = rofs_close,
	.open_root = rofs_open_root,
	.file_name = rofs_file_name_handle,
	.full_path = rofs_full_path,
	.is_dir_handle = rofs_is_dir_handle,
	.read_dir = rofs_read_dir,
	.length = rofs_length,
	.seek = rofs_seek,
	.read = rofs_read,
	.read_fully = rofs_read_fully
};
