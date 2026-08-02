/*
 * Fuzz harness for compile_diag_parse_line().
 *
 * The parser is pure and calls nothing outside src/compile_parse.c, so this
 * harness needs no editor stubs at all -- only the module under test.  The
 * fuzz input is split into lines at '\n', matching how compiler/make output
 * actually arrives, and every line is fed to one persistent parser so
 * Entering/Leaving directory nesting is exercised across the whole input,
 * not just within one line.
 */
#include "../src/compile_parse.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_FUZZ_LINES 512

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;
	const char *base = (const char *)data;
	size_t pos = 0;
	size_t lines = 0;

	compile_diag_parser_init(&parser, "/build", 6);
	compile_diag_store_init(&store);

	while (pos < size && lines < MAX_FUZZ_LINES) {
		size_t start = pos;

		while (pos < size && base[pos] != '\n') {
			pos++;
		}
		compile_diag_parse_line(
		    &parser, &store, base + start, pos - start);
		lines++;
		if (pos < size) {
			pos++;
		}
	}

	return 0;
}
