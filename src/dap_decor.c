/* The debugger's marks, projected into whatever buffers happen to be open.
 * src/dap_decor.h has the contract; this is the projection.
 */

#include "dap_decor.h"

#include "dap_breakpoint.h"
#include "dap_exec.h"
#include "decor.h"
#include "def.h"
#include "event.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Priorities within one row.  A breakpoint outranks the stopped line so
 * that column one still says "there is a breakpoint here" while the rest of
 * the row says "and this is where you are". */
#define DAP_DECOR_PRIORITY_CURRENT 1
#define DAP_DECOR_PRIORITY_PENDING 2
#define DAP_DECOR_PRIORITY_BREAKPOINT 3

static struct kg_decor_handle painted[DAP_DECOR_MAX];
static size_t painted_count;
static struct kg_event_subscriber_token event_token;

/* The buffer visiting `path`, or NULL.  Asked by the canonical path the
 * breakpoint table keys on, so one file is one buffer however the user
 * reached it. */
static struct editor_buffer *buffer_visiting(const char *path)
{
	char resolved[PATH_MAX];
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];

		if (!b->active || b->no_file || !b->filename
		    || b->filename[0] == '\0') {
			continue;
		}
		if (realpath(b->filename, resolved)
		    && strcmp(resolved, path) == 0) {
			return b;
		}
	}
	return NULL;
}

static void paint(struct editor_buffer *b, int row, int end_col,
    enum kg_decor_face face, uint8_t priority)
{
	size_t start, end;

	if (painted_count >= DAP_DECOR_MAX || row < 0 || row >= b->numrows) {
		return;
	}
	start = buffer_row_col_to_position(b, row, 0);
	end = buffer_row_col_to_position(b, row, end_col);
	/* LEFT gravity at the start and RIGHT at the end, the opposite of a
	 * search match: text typed at either edge of the marked line is part
	 * of that line, and the mark should grow with it. */
	painted[painted_count] = kg_decor_create(b, start, end,
	    KG_MARKER_GRAV_LEFT, KG_MARKER_GRAV_RIGHT, face, priority, false);
	if (kg_decor_resolve(painted[painted_count], NULL) == KG_DECOR_OK) {
		painted_count++;
	}
}

static void paint_source(size_t source)
{
	const char *path = dap_breakpoint_source_path(source);
	struct editor_buffer *b = path ? buffer_visiting(path) : NULL;
	size_t n = dap_breakpoint_source_length(source);
	size_t i;

	if (!b) {
		return;
	}
	for (i = 0; i < n; i++) {
		struct dap_breakpoint_info info;

		if (!dap_breakpoint_get(source, i, &info) || info.line <= 0) {
			continue;
		}
		/* One character of gutter, and the whole answer to "is this
		 * breakpoint real": the adapter's own verdict. */
		paint(b, info.line - 1, 1,
		    info.verified ? KG_DECOR_FACE_BREAKPOINT
				  : KG_DECOR_FACE_BREAKPOINT_PENDING,
		    info.verified ? DAP_DECOR_PRIORITY_BREAKPOINT
				  : DAP_DECOR_PRIORITY_PENDING);
	}
}

static void paint_current(void)
{
	char path[PATH_MAX];
	struct editor_buffer *b;
	int line = 0;

	if (!dap_exec_current_location(path, sizeof(path), &line)) {
		return;
	}
	b = buffer_visiting(path);
	if (!b || line <= 0 || line > b->numrows) {
		return;
	}
	paint(b, line - 1, b->row[line - 1].size, KG_DECOR_FACE_DEBUG_CURRENT,
	    DAP_DECOR_PRIORITY_CURRENT);
}

static void unpaint_all(void)
{
	size_t i;

	for (i = 0; i < painted_count; i++) {
		(void)kg_decor_delete(painted[i]);
	}
	painted_count = 0;
}

/* A buffer being killed takes its own marks with it, and only its own: the
 * decorations in every other buffer are still exactly right, and repainting
 * them would be work for no change. */
static void unpaint_buffer(struct kg_buffer_handle handle)
{
	size_t i = 0;

	while (i < painted_count) {
		if (painted[i].buffer.slot == handle.slot
		    && painted[i].buffer.id == handle.id) {
			(void)kg_decor_delete(painted[i]);
			painted[i] = painted[--painted_count];
			continue;
		}
		i++;
	}
}

void dap_decor_refresh(void)
{
	size_t sources = dap_breakpoint_source_count();
	size_t i;

	unpaint_all();
	for (i = 0; i < sources; i++) {
		paint_source(i);
	}
	paint_current();
}

static enum kg_event_cb_status on_buffer_event(
    const struct kg_event *ev, enum kg_event_resolution resolution, void *ctx)
{
	(void)ctx;
	if (ev->kind == KG_EVENT_BUFFER_KILLING) {
		unpaint_buffer(ev->payload.buffer_life.buffer);
	} else if (ev->kind == KG_EVENT_BUFFER_OPENED
	    && resolution == KG_EVENT_RESOLVED_LIVE) {
		dap_decor_refresh();
	}
	return KG_EVENT_CB_CONTINUE;
}

void dap_decor_init(void)
{
	(void)kg_event_unsubscribe(event_token);
	event_token = kg_event_subscribe(
	    (1u << KG_EVENT_BUFFER_OPENED) | (1u << KG_EVENT_BUFFER_KILLING),
	    on_buffer_event, NULL);
}

void dap_decor_reset(void)
{
	unpaint_all();
	(void)kg_event_unsubscribe(event_token);
	event_token = KG_EVENT_SUBSCRIBER_NONE;
}

size_t dap_decor_count(void) { return painted_count; }
