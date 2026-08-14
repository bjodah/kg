/* ======================== Terminal mouse reporting ======================== */

#include <stdlib.h>
#include <string.h>

#include "bufhandle.h"
#include "def.h"
#include "marker.h"
#include "mouse.h"
#include "vgeom.h"
#include "winmgr.h"

/* DECSET 1000 (button press and release), 1002 (motion reports while a
 * button is held, which is what makes a drag visible) and 1006 (the SGR
 * encoding, the only one whose coordinates are not capped at column
 * 223).  A terminal that knows none of them silently ignores the whole
 * string. */
#define MOUSE_ON "\x1b[?1000;1002;1006h"
#define MOUSE_OFF "\x1b[?1000;1002;1006l"

/* Emacs' terminal default for mouse-wheel-scroll-amount. */
#define MOUSE_WHEEL_LINES 5

/* Bits an SGR button number carries besides the button itself: Shift,
 * Meta, Ctrl and the motion flag.  kg reads only the motion one, and
 * takes all four off before comparing against a button. */
#define MOUSE_MOD_BITS (4 | 8 | 16 | 32)
#define MOUSE_MOTION_BIT 32

/* A field stops growing here rather than overflowing.  Far past any real
 * terminal, and window_at() throws such a position out anyway. */
#define MOUSE_FIELD_MAX 100000

/* -1 until TERM has been consulted; 0/1 after, and after the
 * xterm-mouse-mode command has had its say. */
static int mouse_wanted = -1;
/* Whether the request string is currently out on the wire.  Distinct
 * from `mouse_wanted`: C-z takes the request back without turning the
 * mode off, so the resume can put it out again. */
static bool mouse_reporting;

static struct kg_mouse_report pending;
static bool pending_valid;

/* The left button is down, and where.  `window` is the slot the press
 * landed in: a drag that wanders out of it keeps extending the region
 * there rather than jumping to whatever it wandered into. */
static struct {
	bool active;
	bool marked;
	int window;
} drag;

bool kg_mouse_term_reports(const char *term)
{
	return term && term[0] != '\0' && strcmp(term, "dumb") != 0
	    && strcmp(term, "unknown") != 0;
}

static bool mouse_wanted_now(void)
{
#ifdef KG_FUZZ
	/* The fuzz harness has no terminal and no TERM worth consulting,
	 * and tty_write() writes nothing there.  Always on, so the decoder
	 * and everything it reaches stays reachable, and reachable the same
	 * way on every box. */
	mouse_wanted = 1;
#endif
	if (mouse_wanted < 0) {
		mouse_wanted = kg_mouse_term_reports(getenv("TERM"));
	}
	return mouse_wanted != 0;
}

bool kg_mouse_enabled(void) { return mouse_wanted_now(); }

void kg_mouse_start(void)
{
	if (!mouse_wanted_now() || mouse_reporting) {
		return;
	}
	(void)tty_write(MOUSE_ON, sizeof(MOUSE_ON) - 1);
	mouse_reporting = true;
}

void kg_mouse_stop(void)
{
	if (!mouse_reporting) {
		return;
	}
	(void)tty_write(MOUSE_OFF, sizeof(MOUSE_OFF) - 1);
	mouse_reporting = false;
	drag.active = false;
	pending_valid = false;
}

void kg_mouse_toggle(void)
{
	bool on = !mouse_wanted_now();

	mouse_wanted = on;
	if (on && editor.rawmode) {
		kg_mouse_start();
	} else {
		kg_mouse_stop();
	}
	editor_set_status_message(
	    "Xterm-Mouse mode %s", on ? "enabled" : "disabled");
}

/* ---- Decoding ---------------------------------------------------------- */

/* One decimal field of a report.  Digits and nothing else; the running
 * value saturates rather than overflowing. */
static bool parse_field(const char **p, int *out)
{
	const char *s = *p;
	int v = 0;

	if (*s < '0' || *s > '9') {
		return false;
	}
	while (*s >= '0' && *s <= '9') {
		if (v <= MOUSE_FIELD_MAX) {
			v = v * 10 + (*s - '0');
		}
		s++;
	}
	*p = s;
	*out = v;
	return true;
}

bool kg_mouse_parse_sgr(
    const char *params, char final_byte, struct kg_mouse_report *out)
{
	const char *p = params;
	int code;

	if (final_byte != 'M' && final_byte != 'm') {
		return false;
	}
	if (!parse_field(&p, &code) || *p++ != ';') {
		return false;
	}
	if (!parse_field(&p, &out->col) || *p++ != ';') {
		return false;
	}
	if (!parse_field(&p, &out->row) || *p != '\0') {
		return false;
	}
	out->motion = (code & MOUSE_MOTION_BIT) != 0;
	out->release = final_byte == 'm';
	out->button = code & ~MOUSE_MOD_BITS;
	return true;
}

void kg_mouse_record(const char *params, char final_byte)
{
	pending_valid = kg_mouse_parse_sgr(params, final_byte, &pending);
}

/* ---- Geometry ---------------------------------------------------------- */

static int clamp_int(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* The active window whose *text* area covers 1-based terminal
 * (row, col), or -1.  A mode line, the echo area and the separator
 * column between two side-by-side windows all answer -1: v1 ignores a
 * click that did not land on buffer text. */
static int window_at(int row, int col)
{
	int i;

	for (i = 0; i < MAX_WINDOWS; i++) {
		const struct editor_window *w = &winlist[i];

		if (!w->active || row < w->y || row >= w->y + w->h) {
			continue;
		}
		if (col >= w->x && col < w->x + w->w) {
			return i;
		}
	}
	return -1;
}

/* Give the window in `slot` the focus, the way C-x o does: the window
 * already holds its own view, so all that changes is which buffer is
 * current. */
static void mouse_select_window(int slot)
{
	if (slot == win_current) {
		return;
	}
	win_current = slot;
	buf_select(win_buffer_slot(wcur()));
}

/* Move point in the current window to the click at 1-based terminal
 * (row, col).
 *
 * The terminal column is a DISPLAY column and point is a chars offset
 * (doc/coordinates.md rows 1 and 3), so it is converted with
 * editor_visual_col() / editor_chars_col_at_visual() -- the pair
 * display.c places the cursor with, and the inverse of each other at
 * glyph starts.  Using it as a byte offset would land point in the wrong
 * place on every line holding a tab or a wide glyph, which is exactly
 * what a click aims at.  `coloff` is a chars offset too, so the window's
 * left edge is measured in display columns before the click's own
 * offset is added to it. */
static void mouse_set_point(int row, int col)
{
	struct editor_window *w = wcur();
	int y = clamp_int(row - w->y, 0, w->h > 0 ? w->h - 1 : 0);
	int dx = clamp_int(col - w->x, 0, win_text_width(w) - 1);
	erow *r;
	int filerow, chars;

	if (bcur()->visual_line_mode) {
		goto_visual_row_col(w->rowoff_visual + y, dx);
		return;
	}
	filerow = w->rowoff + y;
	if (filerow >= bcur()->numrows) {
		filerow = bcur()->numrows - 1;
	}
	if (filerow < 0) {
		editor_cursor_goto(0, 0);
		return;
	}
	r = &bcur()->row[filerow];
	chars = editor_chars_col_at_visual(r,
	    editor_visual_col(r, w->coloff, buf_display_options(bcur())) + dx,
	    buf_display_options(bcur()));
	/* editor_chars_col_at_visual() answers a click past the row's last
	 * glyph in virtual space; point only lives there in rectangle mark
	 * mode, so a click past end-of-line lands at end-of-line. */
	if (chars > r->size) {
		chars = r->size;
	}
	KG_ASSERT_CHARS_OFF(r, chars);
	editor_cursor_goto(filerow, chars);
}

/* ---- Behaviours -------------------------------------------------------- */

/* Wheel scrolling in visual-line mode, where the drawn viewport is
 * rowoff_visual rather than rowoff.  Point is pulled back to the edge it
 * left, and goto_visual_row_col() is what turns that visual row back
 * into a point. */
static void mouse_scroll_visual(struct editor_window *w, int lines)
{
	struct editor_buffer *b = bcur();
	int total = get_total_visual_rows(w, b);
	int vrow = get_visual_row(w, b, w->rowoff + w->cy, w->coloff + w->cx);

	w->rowoff_visual
	    = clamp_int(w->rowoff_visual + lines, 0, total > 0 ? total - 1 : 0);
	vrow = clamp_int(vrow, w->rowoff_visual,
	    w->rowoff_visual + (w->h > 0 ? w->h - 1 : 0));
	goto_visual_row_col(vrow, 0);
}

/* Scroll the current window `lines` rows (negative is towards the start
 * of the buffer) without insisting point come along.  Point keeps its
 * place in the buffer until the window would leave it behind, and is
 * then dragged to the edge it left -- kg's own line motion at a window
 * edge, and what Emacs does on a terminal. */
static void mouse_scroll(int lines)
{
	struct editor_window *w = wcur();
	int filerow = w->rowoff + w->cy;
	int last = bcur()->numrows > 0 ? bcur()->numrows - 1 : 0;

	w->desired_visual_col = -1;
	if (bcur()->visual_line_mode) {
		mouse_scroll_visual(w, lines);
		return;
	}
	w->rowoff = clamp_int(w->rowoff + lines, 0, last);
	w->cy = clamp_int(filerow - w->rowoff, 0, w->h > 0 ? w->h - 1 : 0);
	editor_snap_cx_to_row();
}

/* A press.  Wheel notches are presses too in the SGR encoding, and are
 * the one kind that does not move point. */
static void mouse_button_down(const struct kg_mouse_report *ev)
{
	int slot = window_at(ev->row, ev->col);

	drag.active = false;
	if (slot < 0) {
		return; /* mode line, echo area or separator: ignored in v1 */
	}
	mouse_select_window(slot);
	if (ev->button == KG_MOUSE_WHEEL_UP) {
		mouse_scroll(-MOUSE_WHEEL_LINES);
		return;
	}
	if (ev->button == KG_MOUSE_WHEEL_DOWN) {
		mouse_scroll(MOUSE_WHEEL_LINES);
		return;
	}
	if (ev->button != KG_MOUSE_BUTTON_LEFT) {
		return; /* middle and right do nothing in v1 */
	}
	mouse_set_point(ev->row, ev->col);
	wcur()->desired_visual_col = -1;
	drag.active = true;
	drag.marked = false;
	drag.window = slot;
}

/* Motion with the left button held.  The press position becomes the mark
 * the first time the pointer actually moves, so a plain click leaves the
 * mark where the user put it; point then follows the pointer, and the
 * region kg already draws for a mark does the rest. */
static void mouse_drag_to(const struct kg_mouse_report *ev)
{
	struct editor_window *w = wcur();

	if (!drag.active || drag.window != win_current) {
		return;
	}
	if (!drag.marked
	    && !kg_mark_set_row_col(
		bcur(), w->rowoff + w->cy, w->coloff + w->cx)) {
		return;
	}
	drag.marked = true;
	bcur()->rect_mode = 0;
	bcur()->shift_select = 0;
	mouse_set_point(ev->row, ev->col);
	bcur()->mark_highlight = 1;
	wcur()->desired_visual_col = -1;
}

void kg_mouse_handle_pending(void)
{
	struct kg_mouse_report ev = pending;

	if (!pending_valid) {
		return;
	}
	pending_valid = false;
	/* The mode is off.  The report was still consumed whole by the
	 * decoder, so none of its bytes can reach the buffer as text -- it
	 * simply moves nothing. */
	if (!kg_mouse_enabled()) {
		return;
	}
	if (ev.release) {
		drag.active = false;
		return;
	}
	if (ev.motion) {
		mouse_drag_to(&ev);
		return;
	}
	mouse_button_down(&ev);
}
