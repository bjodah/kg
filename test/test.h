#ifndef TEST_H
#define TEST_H

#include <stdio.h>

extern int tests_run;
extern int tests_failed;

/* Shared helper: free all rows and the row array of the current buffer. */
void free_all_rows(void);

/* Shared helper: free the current buffer's rows and undo history and clear
 * every per-buffer field. */
void reset_current_buffer(void);

#define CHECK(cond)                                                            \
	do {                                                                   \
		tests_run++;                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__,        \
			    __LINE__, #cond);                                  \
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
