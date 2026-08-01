#ifndef TEST_H
#define TEST_H

#include "../src/marker.h"
#include <stdio.h>

struct editor_buffer;

extern int tests_run;
extern int tests_failed;

/* Shared helper: free all rows and the row array of the current buffer. */
void free_all_rows(void);

/* Shared helper: free the current buffer's rows and undo history and clear
 * every per-buffer field. */
void reset_current_buffer(void);

/* Shared helper: put the selected window's point, scroll and goal column
 * back to the top of the buffer. */
void reset_current_view(void);

/* Marker-backed mark helpers for tests that need to arrange or inspect a
 * buffer's region without reaching into marker-store internals. */
static inline int test_set_mark(struct editor_buffer *b, int row, int col)
{
	return kg_mark_set_row_col(b, row, col);
}

static inline int test_mark_row(const struct editor_buffer *b)
{
	int row, col;

	return kg_mark_get_row_col(b, &row, &col) ? row : -1;
}

static inline int test_mark_col(const struct editor_buffer *b)
{
	int row, col;

	return kg_mark_get_row_col(b, &row, &col) ? col : -1;
}

#define CHECK(cond)                                                            \
	do {                                                                   \
		tests_run++;                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__,        \
			    __LINE__, #cond);                                  \
			tests_failed++;                                        \
		}                                                              \
	} while (0)

/* CHECK with a printf-style note, for assertions inside a loop where the
 * condition alone does not say which element failed. */
#define CHECKF(cond, ...)                                                      \
	do {                                                                   \
		tests_run++;                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "  FAIL %s:%d: %s\n       ", __FILE__, \
			    __LINE__, #cond);                                  \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			tests_failed++;                                        \
		}                                                              \
	} while (0)

#define RUN(fn)                                                                \
	do {                                                                   \
		printf("  %s\n", #fn);                                         \
		fn();                                                          \
	} while (0)

static inline int test_summary(void) { return tests_failed ? 1 : 0; }

#endif /* TEST_H */
