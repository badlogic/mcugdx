#define TINYFILES_IMPL
#include "tinyfiles.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
	uint32_t capacity;
	uint32_t num_files;
	tfFILE *files;
} files_t;

void process_file(tfFILE *file, void *udata) {
	files_t *files = (files_t *) udata;

	if (!strcmp(file->name, ".") || !strcmp(file->name, "..") || !file->is_reg) {
		return;
	}

	if (files->num_files == files->capacity) {
		files->capacity = (uint32_t) (files->capacity * 1.5f);
		files->files = (tfFILE *) realloc(files->files, sizeof(tfFILE) * 256);
	}

	files->files[files->num_files++] = *file;
}

int main(int argc, char **argv) {
	if (argc != 3) {
		printf("Usage: rofs <output-file> <input-dir>\n");
		return -1;
	}

	char *output = argv[1];
	char *input = argv[2];
	files_t files = {
			.capacity = 256,
			.num_files = 0,
			.files = (tfFILE *) (malloc(sizeof(tfFILE) * 256))};

	tfTraverse(input, process_file, &files);

	FILE *out = fopen(output, "wb");
	fprintf(out, "%i\n", files.num_files);

	uint32_t offset = 0;
    uint32_t input_len = strlen(input);
	for (uint32_t i = 0; i < files.num_files; i++) {
		tfFILE *file = &files.files[i];
		fprintf(out, "%s\n", file->path + input_len + 1);
		fprintf(out, "%i\n", offset);
		fprintf(out, "%i\n", file->size);
		offset += file->size;
	}

	for (uint32_t i = 0; i < files.num_files; i++) {
		tfFILE *file = &files.files[i];
		FILE *in = fopen(file->path, "rb");
		char buffer[4096];
		size_t bytes_read;
		while ((bytes_read = fread(buffer, 1, sizeof(buffer), in)) > 0) {
			fwrite(buffer, 1, bytes_read, out);
		}
		fclose(in);
	}
	fclose(out);

	printf("Processed %u files\n", files.num_files);

	free(files.files);
}