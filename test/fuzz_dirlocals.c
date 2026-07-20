/*
 * Fuzz harness for dirlocals_parse() – the .dir-locals.el S-expression parser.
 *
 * This is a pure parser: give it a string and it fills a struct.
 * Perfect target because it handles nested quoting, escaping,
 * parentheses, comments, and depth/token limits.
 */
#include "../src/localvars.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct local_settings out;

	/* dirlocals_parse caps at 64 KiB internally; no need to truncate. */
	dirlocals_parse((const char *)data, size, &out);
	return 0;
}
