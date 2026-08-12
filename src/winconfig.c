/* Atomic window-configuration restore and bulk grid construction. */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bufhandle.h"
#include "bufmgr_internal.h"
#include "def.h"
#include "event.h"
#include "vgeom.h"
#include "winmgr.h"
#include "winmgr_internal.h"

#define WIN_LAYOUT_EVENTS_MAX (MAX_WINDOWS * 2 + 1)

struct win_layout_plan {
	struct kg_window_snapshot windows[MAX_WINDOWS];
	int count;
	int selected;
	int create_scratch;
	int restore_fields;
};

static int window_handles_equal(
    struct kg_window_handle a, struct kg_window_handle b)
{
	return a.slot == b.slot && a.id == b.id && a.generation == b.generation;
}

static int snapshot_handle_shape_valid(const struct kg_window_snapshot *s)
{
	return s->slot >= 0 && s->slot < MAX_WINDOWS && s->buffer.slot >= 0
	    && s->buffer.slot < MAX_BUFFERS && s->buffer.id != 0
	    && s->buffer.generation != 0 && s->window.id != 0
	    && s->window.generation != 0 && s->window.slot == s->slot;
}

static int snapshot_view_shape_valid(const struct kg_window_snapshot *s)
{
	return s->cx >= 0 && s->cy >= 0 && s->rowoff >= 0 && s->coloff >= 0
	    && s->rowoff_visual >= 0 && s->desired_visual_col >= -1
	    && s->col_group >= 0 && s->cy <= INT_MAX - s->rowoff
	    && s->cx <= INT_MAX - s->coloff;
}

static int snapshot_numbers_valid(const struct kg_window_snapshot *s)
{
	return snapshot_handle_shape_valid(s) && snapshot_view_shape_valid(s);
}

static int configuration_valid(
    const struct kg_window_configuration *configuration)
{
	int selected = 0;

	if (!configuration || !configuration->valid || configuration->count < 1
	    || configuration->count > MAX_WINDOWS) {
		return 0;
	}
	for (int i = 0; i < configuration->count; i++) {
		const struct kg_window_snapshot *s = &configuration->windows[i];

		if (!snapshot_numbers_valid(s)) {
			return 0;
		}
		selected += window_handles_equal(
		    s->window, configuration->selected_window);
		for (int j = 0; j < i; j++) {
			if (configuration->windows[j].slot == s->slot
			    || window_handles_equal(
				s->window, configuration->windows[j].window)) {
				return 0;
			}
		}
	}
	return selected == 1;
}

void win_configuration_clear(struct kg_window_configuration *configuration)
{
	if (configuration) {
		memset(configuration, 0, sizeof(*configuration));
	}
}

static struct kg_window_snapshot snapshot_window(int slot)
{
	struct editor_window *w = &winlist[slot];

	return (struct kg_window_snapshot) {
		.buffer = w->buf,
		.window = win_handle(slot),
		.slot = slot,
		.cx = w->cx,
		.cy = w->cy,
		.rowoff = w->rowoff,
		.coloff = w->coloff,
		.rowoff_visual = w->rowoff_visual,
		.desired_visual_col = w->desired_visual_col,
		.col_group = w->col_group,
	};
}

void win_configuration_save(struct kg_window_configuration *configuration)
{
	int count = 0;

	if (!configuration) {
		return;
	}
	win_configuration_clear(configuration);
	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (winlist[i].active) {
			configuration->windows[count++] = snapshot_window(i);
		}
	}
	if (count != win_count || win_current < 0 || win_current >= MAX_WINDOWS
	    || !winlist[win_current].active) {
		win_configuration_clear(configuration);
		return;
	}
	configuration->selected_window = win_handle(win_current);
	configuration->count = count;
	configuration->valid = count > 0;
}

static enum kg_window_layout_result restore_plan(
    const struct kg_window_configuration *configuration,
    struct win_layout_plan *plan)
{
	int selected_saved = -1;

	memset(plan, 0, sizeof(*plan));
	plan->restore_fields = 1;
	for (int i = 0; i < configuration->count; i++) {
		const struct kg_window_snapshot *saved
		    = &configuration->windows[i];

		if (!buf_resolve(saved->buffer)) {
			continue;
		}
		plan->windows[plan->count] = *saved;
		if (window_handles_equal(
			saved->window, configuration->selected_window)) {
			selected_saved = plan->count;
		}
		plan->count++;
	}
	if (plan->count > 0) {
		plan->selected = selected_saved >= 0 ? selected_saved : 0;
		return KG_WINDOW_LAYOUT_OK;
	}
	plan->windows[0] = (struct kg_window_snapshot) {
		.slot = configuration->windows[0].slot,
		.desired_visual_col = -1,
	};
	plan->count = 1;
	plan->selected = 0;
	int scratch = buf_find_open("*scratch*");
	if (scratch >= 0) {
		plan->windows[0].buffer = buf_handle(scratch);
		return KG_WINDOW_LAYOUT_OK;
	}
	if (buf_count >= MAX_BUFFERS) {
		return KG_WINDOW_LAYOUT_BUFFER_CAPACITY;
	}
	if (!buf_named_creation_available_for_transaction()) {
		return KG_WINDOW_LAYOUT_IDENTITY_EXHAUSTED;
	}
	plan->create_scratch = 1;
	return KG_WINDOW_LAYOUT_OK;
}

static enum kg_window_layout_result validate_live_layout(int *old_count)
{
	int count = 0;

	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			continue;
		}
		if (!win_buffer(&winlist[i])) {
			return KG_WINDOW_LAYOUT_INVALID;
		}
		count++;
	}
	if (count != win_count || count < 1) {
		return KG_WINDOW_LAYOUT_INVALID;
	}
	*old_count = count;
	return KG_WINDOW_LAYOUT_OK;
}

static enum kg_window_layout_result validate_plan_geometry(
    const struct win_layout_plan *plan)
{
	int groups[MAX_WINDOWS], counts[MAX_WINDOWS] = { 0 };
	int group_count = 0, max_rows = 0;

	for (int i = 0; i < plan->count; i++) {
		int group = plan->windows[i].col_group;
		int found = -1;

		for (int j = 0; j < group_count; j++) {
			if (groups[j] == group) {
				found = j;
				break;
			}
		}
		if (found < 0) {
			found = group_count;
			groups[group_count++] = group;
		}
		counts[found]++;
		if (counts[found] > max_rows) {
			max_rows = counts[found];
		}
	}
	return win_total_rows >= 1 + 2 * max_rows
		&& win_total_cols >= 2 * group_count - 1
	    ? KG_WINDOW_LAYOUT_OK
	    : KG_WINDOW_LAYOUT_TOO_SMALL;
}

/* Column groups are equivalence labels, not externally stable identities.
 * Public configurations may contain any nonnegative int, including INT_MAX;
 * rewrite each distinct label in first-appearance order so later ordinary
 * splits can safely allocate max_group + 1. */
static void canonicalize_column_groups(struct win_layout_plan *plan)
{
	int labels[MAX_WINDOWS];
	int label_count = 0;

	for (int i = 0; i < plan->count; i++) {
		int label = plan->windows[i].col_group;
		int canonical = -1;

		for (int j = 0; j < label_count; j++) {
			if (labels[j] == label) {
				canonical = j;
				break;
			}
		}
		if (canonical < 0) {
			canonical = label_count;
			labels[label_count++] = label;
		}
		plan->windows[i].col_group = canonical;
	}
}

static void release_reservations(
    struct kg_event_reservation reservations[], int count)
{
	for (int i = 0; i < count; i++) {
		kg_event_release_reservation(&reservations[i]);
	}
}

static void deactivate_old_windows(
    struct kg_event_reservation reservations[], int offset)
{
	int event = offset;

	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			continue;
		}
		(void)buf_view_detach_reserved_for_transaction(
		    &winlist[i], &reservations[event++]);
		winlist[i].active = 0;
		winlist[i].generation++;
	}
}

static void restore_view_fields(
    struct editor_window *w, const struct kg_window_snapshot *snapshot)
{
	w->cx = snapshot->cx;
	w->cy = snapshot->cy;
	w->rowoff = snapshot->rowoff;
	w->coloff = snapshot->coloff;
	w->rowoff_visual = snapshot->rowoff_visual;
	w->desired_visual_col = snapshot->desired_visual_col;
}

static struct kg_window_snapshot capture_view_fields(
    const struct editor_window *w)
{
	return (struct kg_window_snapshot) {
		.cx = w->cx,
		.cy = w->cy,
		.rowoff = w->rowoff,
		.coloff = w->coloff,
		.rowoff_visual = w->rowoff_visual,
		.desired_visual_col = w->desired_visual_col,
	};
}

static void normalize_visual_offset(struct editor_window *w)
{
	struct editor_buffer *b = win_buffer(w);
	int height = w->h > 0 ? w->h : 0;
	int total = get_total_visual_rows(w, b);
	int last = total > 0 ? total - 1 : 0;

	if (w->rowoff_visual > last) {
		w->rowoff_visual = last;
	}
	if (w->rowoff_visual > INT_MAX - height) {
		w->rowoff_visual = INT_MAX - height;
	}
	/* get_total_visual_rows() may have built an index merely to validate
	 * the restored offset.  Layout restore promises no copied or retained
	 * geometry; the renderer may rebuild this lazily on its first paint. */
	vgeom_window_free(w);
}

static void build_new_windows(const struct win_layout_plan *plan,
    struct kg_event_reservation reservations[], int offset)
{
	for (int i = 0; i < plan->count; i++) {
		const struct kg_window_snapshot *snapshot = &plan->windows[i];
		struct editor_window *w = &winlist[snapshot->slot];
		uint64_t generation = w->generation;

		free(w->vgeom);
		memset(w, 0, sizeof(*w));
		w->generation = generation;
		w->active = 1;
		w->col_group = snapshot->col_group;
		(void)win_claim_slot_for_layout(snapshot->slot);
		(void)buf_view_attach_reserved_for_transaction(
		    w, snapshot->buffer.slot, &reservations[offset + i]);
		if (plan->restore_fields) {
			restore_view_fields(w, snapshot);
		}
	}
}

static enum kg_window_layout_result commit_layout(struct win_layout_plan *plan)
{
	struct kg_event_reservation reservations[WIN_LAYOUT_EVENTS_MAX];
	struct kg_window_snapshot views[MAX_WINDOWS];
	int old_count, open_events = plan->create_scratch ? 1 : 0;
	enum kg_window_layout_result result = validate_live_layout(&old_count);
	int event_count;

	if (result != KG_WINDOW_LAYOUT_OK) {
		return result;
	}
	canonicalize_column_groups(plan);
	event_count = open_events + old_count + plan->count;
	result = validate_plan_geometry(plan);
	if (result != KG_WINDOW_LAYOUT_OK) {
		return result;
	}
	if (!win_identities_available(plan->count)) {
		return KG_WINDOW_LAYOUT_IDENTITY_EXHAUSTED;
	}
	if (!kg_event_reserve_lifecycle_batch(
		reservations, (size_t)event_count)) {
		return KG_WINDOW_LAYOUT_EVENT_CAPACITY;
	}
	if (plan->create_scratch) {
		plan->windows[0].buffer
		    = buf_create_named_reserved_for_transaction(
			"*scratch*", &reservations[0]);
		if (!buf_resolve(plan->windows[0].buffer)) {
			release_reservations(reservations, event_count);
			return KG_WINDOW_LAYOUT_ALLOCATION_FAILED;
		}
	}
	deactivate_old_windows(reservations, open_events);
	win_count = plan->count;
	build_new_windows(plan, reservations, open_events + old_count);
	win_current = plan->windows[plan->selected].slot;
	buf_current = plan->windows[plan->selected].buffer.slot;
	editor_refresh_readonly_state();
	for (int i = 0; i < plan->count; i++) {
		views[i] = capture_view_fields(&winlist[plan->windows[i].slot]);
	}
	win_reflow();
	for (int i = 0; i < plan->count; i++) {
		struct editor_window *w = &winlist[plan->windows[i].slot];

		restore_view_fields(w, &views[i]);
		buf_view_clamp_for_transaction(w);
		normalize_visual_offset(w);
	}
	win_check_handles();
	KG_STATE_CHECK("window layout");
	return KG_WINDOW_LAYOUT_OK;
}

enum kg_window_layout_result win_configuration_restore(
    const struct kg_window_configuration *configuration)
{
	struct win_layout_plan plan;
	enum kg_window_layout_result result;

	if (!configuration_valid(configuration)) {
		return KG_WINDOW_LAYOUT_INVALID;
	}
	result = restore_plan(configuration, &plan);
	return result == KG_WINDOW_LAYOUT_OK ? commit_layout(&plan) : result;
}

static int grid_arguments_valid(int column_count, const int rows_per_column[],
    const struct kg_buffer_handle buffers[], int buffer_count,
    int selected_index)
{
	return rows_per_column && buffers && column_count >= 1
	    && column_count <= MAX_WINDOWS && buffer_count >= 1
	    && buffer_count <= MAX_WINDOWS && selected_index >= 0
	    && selected_index < buffer_count;
}

static enum kg_window_layout_result validate_grid(int column_count,
    const int rows_per_column[], const struct kg_buffer_handle buffers[],
    int buffer_count, int selected_index)
{
	int count = 0, max_rows = 0;

	if (!grid_arguments_valid(column_count, rows_per_column, buffers,
		buffer_count, selected_index)) {
		return KG_WINDOW_LAYOUT_INVALID;
	}
	for (int i = 0; i < column_count; i++) {
		if (rows_per_column[i] < 1
		    || count > MAX_WINDOWS - rows_per_column[i]) {
			return KG_WINDOW_LAYOUT_WINDOW_CAPACITY;
		}
		count += rows_per_column[i];
		if (rows_per_column[i] > max_rows) {
			max_rows = rows_per_column[i];
		}
	}
	if (count != buffer_count) {
		return KG_WINDOW_LAYOUT_INVALID;
	}
	if (win_total_rows < 1 + 2 * max_rows
	    || win_total_cols < 2 * column_count - 1) {
		return KG_WINDOW_LAYOUT_TOO_SMALL;
	}
	return KG_WINDOW_LAYOUT_OK;
}

static enum kg_window_layout_result grid_buffer_valid(
    const struct kg_buffer_handle buffers[], int index)
{
	if (!buf_resolve(buffers[index])) {
		return KG_WINDOW_LAYOUT_BUFFER_GONE;
	}
	for (int i = 0; i < index; i++) {
		if (buf_resolve(buffers[i]) == buf_resolve(buffers[index])) {
			return KG_WINDOW_LAYOUT_DUPLICATE_BUFFER;
		}
	}
	return KG_WINDOW_LAYOUT_OK;
}

static void make_grid_plan(struct win_layout_plan *plan, int column_count,
    const int rows_per_column[], const struct kg_buffer_handle buffers[],
    int buffer_count, int selected_index)
{
	int index = 0;

	memset(plan, 0, sizeof(*plan));
	for (int column = 0; column < column_count; column++) {
		for (int row = 0; row < rows_per_column[column];
		    row++, index++) {
			plan->windows[index] = (struct kg_window_snapshot) {
				.buffer = buffers[index],
				.slot = index,
				.desired_visual_col = -1,
				.col_group = column,
			};
		}
	}
	plan->count = buffer_count;
	plan->selected = selected_index;
	plan->restore_fields = 0;
}

enum kg_window_layout_result win_arrange_grid(int column_count,
    const int rows_per_column[], const struct kg_buffer_handle buffers[],
    int buffer_count, int selected_index)
{
	struct win_layout_plan plan;
	enum kg_window_layout_result result = validate_grid(column_count,
	    rows_per_column, buffers, buffer_count, selected_index);

	if (result != KG_WINDOW_LAYOUT_OK) {
		return result;
	}
	for (int i = 0; i < buffer_count; i++) {
		result = grid_buffer_valid(buffers, i);
		if (result != KG_WINDOW_LAYOUT_OK) {
			return result;
		}
	}
	make_grid_plan(&plan, column_count, rows_per_column, buffers,
	    buffer_count, selected_index);
	return commit_layout(&plan);
}
