/* test.c — global definitions for the test harness */

#include "test.h"
#include "../src/def.h"
#include <stdlib.h>

int tests_run = 0;
int tests_failed = 0;

void free_all_rows(void)
{
	int i;

	for (i = 0; i < editor.numrows; i++) {
		editor_free_row(&editor.row[i]);
	}
	free(editor.row);
	/* The row array now carries a capacity (editor_rows_reserve()), so
	 * dropping the rows has to drop it too: a stale capacity over a
	 * freed pointer is what the next editor_insert_row() would write
	 * through. */
	editor.row = NULL;
	editor.numrows = 0;
	editor.row_capacity = 0;
}
