/* test.c — global definitions for the test harness */

#include "test.h"
#include "../src/def.h"
#include <stdlib.h>
#include <string.h>

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

/* Reset the current buffer: drop its rows and its undo history, and clear
 * every field a buffer owns.  Test setups used to get this for free from
 * `memset(&editor, 0, sizeof(editor))`; a buffer's state no longer lives in
 * that struct, so a flag one test set (read-only, say) would otherwise
 * still be set in the next.  The slot keeps its identity: these tests never
 * switch buffers, and handing out a fresh id per setup would only churn. */
void reset_current_buffer(void)
{
	struct editor_buffer *b = bcur();
	struct editor_buffer keep = *b;

	free_all_rows();
	memset(b, 0, sizeof(*b));
	b->id = keep.id;
	b->generation = keep.generation;
	/* The undo chain stays put: setups pair this with undo_free() and
	 * undo_init(), and a stack dropped here would be a stack leaked. */
	b->undostack = keep.undostack;
	b->readonly_override = -1;
	b->active = 1;
}

/* Reset the selected window's view: point, scroll and goal column.  Its
 * geometry is left alone, because setups set that themselves.  Like
 * reset_current_buffer(), this used to be part of what
 * `memset(&editor, 0, sizeof(editor))` did; the view now belongs to the
 * window. */
void reset_current_view(void)
{
	struct editor_window *w = wcur();

	w->cx = 0;
	w->cy = 0;
	w->rowoff = 0;
	w->coloff = 0;
	w->rowoff_visual = 0;
	w->desired_visual_col = -1;
}
