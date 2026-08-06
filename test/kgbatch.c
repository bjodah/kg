/* kgbatch - repeatable, editor-free init-file corpus driver.
 *
 * This deliberately does not become part of make check: it gives an audit a
 * quick way to evaluate complete source files with kg's embedded Lisp. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"

static int evaluate_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	char *source, result[256];
	long length;
	size_t read;

	if (!file) {
		perror(path);
		return 1;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0
	    || fseek(file, 0, SEEK_SET) != 0) {
		perror(path);
		fclose(file);
		return 1;
	}
	source = malloc((size_t)length + 1);
	if (!source) {
		fprintf(stderr, "%s: %s\n", path, strerror(ENOMEM));
		fclose(file);
		return 1;
	}
	read = fread(source, 1, (size_t)length, file);
	fclose(file);
	if (read != (size_t)length) {
		fprintf(stderr, "%s: cannot read file\n", path);
		free(source);
		return 1;
	}
	source[length] = '\0';
	if (kg_lisp_eval_string(
		source, (size_t)length, result, sizeof(result))) {
		fprintf(stderr, "%s: %s\n", path, result);
		free(source);
		return 1;
	}
	printf("%s: %s\n", path, result);
	free(source);
	return 0;
}

int main(int argc, char **argv)
{
	int i;
	int status = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s FILE...\n", argv[0]);
		return 2;
	}
	if (kg_lisp_init()) {
		fprintf(stderr, "cannot initialize Lisp: %s\n",
		    kg_lisp_last_error());
		return 1;
	}
	for (i = 1; i < argc; i++) {
		status |= evaluate_file(argv[i]);
	}
	kg_lisp_shutdown();
	return status;
}
