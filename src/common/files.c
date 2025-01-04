#include "files.h"
#include "rofs.h"

bool mcugdx_rofs_init(void) {
	return rofs_init();
}

mcugdx_file_system_t mcugdx_rofs = {
	.exists = rofs_exists,
	.length = rofs_length,
	.open = rofs_open,
	.close = rofs_close,
	.read = rofs_read,
	.read_fully = rofs_read_fully,
	.seek = rofs_seek,
	.num_files = rofs_num_files,
	.file_name = rofs_file_name
};
