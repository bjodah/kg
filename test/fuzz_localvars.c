/*
 * Fuzz harness for localvars_parse_modeline() and localvars_parse_footer().
 *
 * These parse Emacs-style "-*- key: value -*-" modelines and
 * "Local Variables:" footer blocks out of editor rows.  The fuzz
 * input is split into rows at NUL bytes so the fuzzer can explore
 * multi-line footer blocks, continuation lines, etc.
 */
#include "../src/def.h"
#include "../src/localvars.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Globals required by def.h but unused by the localvars parser. */
struct editor_config editor;
int running = 1;
int global_auto_revert = 0;
int electric_pairs = 0;
int kg_exit_status = 0;

struct editor_buffer buflist[MAX_BUFFERS];
int buf_current = 0;
int buf_count = 1;

struct editor_window winlist[MAX_WINDOWS];
int win_current = 0;
int win_count = 1;
int win_total_rows = 24;
int win_total_cols = 80;

void editor_set_status_message(const char *fmt, ...) { (void)fmt; }

#define MAX_FUZZ_ROWS 128

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	erow rows[MAX_FUZZ_ROWS];
	int nrows = 0;
	struct local_settings out;

	/* Split input into rows at NUL bytes.  Each row gets chars/size
	 * pointing into a mutable copy (the parsers don't modify them). */
	char *copy = malloc(size + 1);
	if (!copy) {
		return 0;
	}
	memcpy(copy, data, size);
	copy[size] = '\0';

	memset(rows, 0, sizeof(rows));

	{
		const char *p = copy;
		const char *end = copy + size;

		while (p < end && nrows < MAX_FUZZ_ROWS) {
			const char *nl = memchr(p, '\0', (size_t)(end - p));
			if (!nl) {
				nl = end;
			}
			rows[nrows].idx = nrows;
			rows[nrows].chars = (char *)p;
			rows[nrows].size = (int)(nl - p);
			/* render/rsize unused by localvars parsers. */
			rows[nrows].render = rows[nrows].chars;
			rows[nrows].rsize = rows[nrows].size;
			nrows++;
			p = nl < end ? nl + 1 : end;
		}
	}

	localvars_parse_modeline(rows, nrows, &out);
	localvars_parse_footer(rows, nrows, &out);

	free(copy);
	return 0;
}
