#ifndef KG_COMPILE_PARSE_H
#define KG_COMPILE_PARSE_H

#include <stdbool.h>
#include <stddef.h>

/* A pure, bounded parser for compiler-style "file:line[:column]"
 * diagnostics, plus the "Entering directory"/"Leaving directory" lines
 * `make` prints around them.  It reads no globals, calls nothing outside
 * this file, and never allocates: every bound below is a fixed array size,
 * so a caller can drive it from a byte stream of any size and any content --
 * this is what makes corpus/fuzz testing possible and lets a later UI slice
 * attach markers to the recorded spans without re-parsing them.
 *
 * Bytes, not C strings: a path is stored as (bytes, length), and nothing in
 * this module calls strlen/strcmp/strchr on a path or applies any encoding
 * assumption to it.  Compiler output is not guaranteed UTF-8 or NUL-free,
 * and a stored path is never NUL-terminated by this module -- a caller that
 * needs a C string makes its own bounded, NUL-terminated copy. */

enum {
	/* Diagnostic records a store keeps before it starts reporting
	 * truncation instead of appending more. */
	KG_COMPILE_DIAG_MAX = 128,
	/* Bytes kept of a file path or a directory-context path; the rest is
	 * silently dropped and the record says so. */
	KG_COMPILE_DIAG_PATH_MAX = 256,
	/* "make[N]: Entering directory" nesting the parser tracks exactly;
	 * deeper recursion freezes the tracked directory at this depth
	 * instead of growing further. */
	KG_COMPILE_DIAG_DIR_STACK_MAX = 32,
};

/* One parsed diagnostic: a location (file, line, optional column) and the
 * directory a relative file path is relative to -- the cwd `make` was
 * in when the line naming it arrived, not the directory kg happens to be
 * running in. */
struct compile_diag {
	char file[KG_COMPILE_DIAG_PATH_MAX];
	size_t file_len;
	bool file_truncated;

	size_t line;
	bool has_column;
	size_t column;

	char cwd[KG_COMPILE_DIAG_PATH_MAX];
	size_t cwd_len;
	bool cwd_truncated;
};

/* The bounded result store.  `truncated` is what distinguishes "the cap was
 * hit" from "nothing matched": count == 0 alone means no diagnostics were
 * found, count == KG_COMPILE_DIAG_MAX with truncated == true means more were
 * found than kept.  rejected_overflow counts lines that matched the
 * file:line[:column] shape but whose line or column number did not fit --
 * refused, not wrapped, and never turned into a record. */
struct compile_diag_store {
	struct compile_diag entries[KG_COMPILE_DIAG_MAX];
	size_t count;
	bool truncated;
	size_t rejected_overflow;
};

struct compile_dir_frame {
	char buf[KG_COMPILE_DIAG_PATH_MAX];
	size_t len;
	bool truncated;
};

/* Streaming state: the directory-context stack "Entering"/"Leaving" lines
 * push and pop, seeded at construction with the directory in effect before
 * the first such line.  malformed_directory_lines counts an Entering/Leaving
 * line whose quoted path could not be read (e.g. an unterminated quote);
 * unbalanced_leaving counts a Leaving line with no matching Entering still
 * open, which leaves the tracked directory unchanged rather than guessing. */
struct compile_diag_parser {
	struct compile_dir_frame stack[KG_COMPILE_DIAG_DIR_STACK_MAX];
	size_t depth;
	size_t unbalanced_leaving;
	size_t stack_overflow_entries;
	size_t malformed_directory_lines;
};

void compile_diag_parser_init(struct compile_diag_parser *parser,
    const char *initial_cwd, size_t initial_cwd_len);
void compile_diag_store_init(struct compile_diag_store *store);

/* Feed one line of raw output, without its terminating newline.  Recognizes
 * a make Entering/Leaving directory line (updates `parser`'s tracked
 * directory, appends no record) or a `file:line[:column]:` diagnostic
 * (appends a record to `store`, tagged with the directory `parser` is
 * currently tracking).  Any other line is silently not a match -- there is
 * no error return, because "not a diagnostic" is not a parse failure. */
void compile_diag_parse_line(struct compile_diag_parser *parser,
    struct compile_diag_store *store, const char *line, size_t len);

#endif
