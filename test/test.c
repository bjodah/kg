/* test.c — global definitions for the test harness */

#include "test.h"
#include "../src/def.h"
#include <stdlib.h>

int tests_run = 0;
int tests_failed = 0;

void free_all_rows(void)
{
	int i;

	for (i = 0; i < bcur()->numrows; i++) {
		editor_free_row(&bcur()->row[i]);
	}
	free(bcur()->row);
	/* The row array carries a capacity (editor_rows_reserve()), so
	 * dropping the rows has to drop it too: a stale capacity over a
	 * freed pointer is what the next editor_insert_row() would write
	 * through.  The dirty flag goes with them: it used to be cleared by
	 * the memset(&editor, ...) every setup() does, and it no longer
	 * lives in that struct. */
	bcur()->row = NULL;
	bcur()->numrows = 0;
	bcur()->row_capacity = 0;
	bcur()->dirty = 0;
}
