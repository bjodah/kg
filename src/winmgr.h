#ifndef KG_WINMGR_H
#define KG_WINMGR_H

#include "bufhandle.h"

/* The fixed window table's capacity.  Window configurations are bounded by
 * the same value, so saving one never allocates and cannot fail halfway. */
#define MAX_WINDOWS 8

/* One caller-owned, copyable snapshot of the live window table.  Buffer and
 * window identities are handles, never borrowed record pointers.  Geometry
 * and the opaque visual-line index are deliberately absent: restore derives
 * geometry from the then-current terminal and rebuilds indexes lazily. */
struct kg_window_snapshot {
	struct kg_buffer_handle buffer;
	struct kg_window_handle window;
	int slot;
	int cx, cy;
	int rowoff, coloff;
	int rowoff_visual;
	int desired_visual_col;
	int col_group;
};

struct kg_window_configuration {
	struct kg_window_snapshot windows[MAX_WINDOWS];
	struct kg_window_handle selected_window;
	int count;
	int valid;
};

/* A configuration owns no heap storage: save overwrites it, clear invalidates
 * it, and restore reads but does not consume it, so a caller may reuse or copy
 * one until it explicitly clears or overwrites it.  col_group values express
 * only which panes share a column; restore canonicalizes distinct nonnegative
 * labels to dense values in first-appearance order. */
void win_configuration_clear(struct kg_window_configuration *configuration);
void win_configuration_save(struct kg_window_configuration *configuration);

enum kg_window_layout_result {
	KG_WINDOW_LAYOUT_OK = 0,
	KG_WINDOW_LAYOUT_INVALID,
	KG_WINDOW_LAYOUT_BUFFER_GONE,
	KG_WINDOW_LAYOUT_DUPLICATE_BUFFER,
	KG_WINDOW_LAYOUT_WINDOW_CAPACITY,
	KG_WINDOW_LAYOUT_BUFFER_CAPACITY,
	KG_WINDOW_LAYOUT_TOO_SMALL,
	KG_WINDOW_LAYOUT_EVENT_CAPACITY,
	KG_WINDOW_LAYOUT_ALLOCATION_FAILED,
	KG_WINDOW_LAYOUT_IDENTITY_EXHAUSTED,
};

enum kg_window_layout_result win_configuration_restore(
    const struct kg_window_configuration *configuration);

/* Replace the layout in one transaction.  `buffers` is flattened in column-
 * major order: rows_per_column[0] handles for the first column, then the
 * second, and so on.  Equal column widths are the v1 reflow policy. */
enum kg_window_layout_result win_arrange_grid(int column_count,
    const int rows_per_column[], const struct kg_buffer_handle buffers[],
    int buffer_count, int selected_index);

void win_init(void);
void win_reflow(void);
void win_split_horizontal(void);
void win_split_vertical(void);
void win_cycle_next(void);
void win_delete_current(void);
void win_delete_others(void);
void win_display_buffer_other_window(int buffer_index);
/* The other direction: stop showing this buffer, without killing it.
 * Declines when the buffer is not on screen, when its window is the only
 * one, or when its window is the current one. */
void win_undisplay_buffer(int buffer_index);
int win_can_display_buffer_other_window(int buffer_index);
void win_position_at_end(int buffer_index);
/* Put point on `row` (0-based) in every window showing this buffer,
 * scrolling as little as it takes to bring it into view. */
void win_position_at_row(int buffer_index, int row);

#endif /* KG_WINMGR_H */
