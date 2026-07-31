/* ============================= Terminal update ============================ */

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bufhandle.h"
#include "def.h"
#include "localvars.h"
#include "perf.h"
#include "syntax.h"

#ifndef ABUF_INIT
#define ABUF_INIT { NULL, 0, 0, 0 }
#endif

/* Welcome banner shown on an empty buffer.  An apothecary's cylindrical
 * brass knob weight stamped with a lower-case "kg", drawn with Unicode
 * box-drawing characters, with a small slogan to its right.  Every row
 * is padded to KG_LOGO_COLS visual columns so the whole block centres
 * at the same column. */
static const char *kg_logo[] = {
	"   ╭────╮    ",
	"   ╰╮  ╭╯    ",
	"╭───╯  ╰───╮ ",
	"│          │ ",
	"│  │       │ ",
	"│  │╱ ╭─╮  │ Your fingers know this",
	"│  │╲ ╰─┤  │   Just enough Emacs",
	"│     ╰─╯  │ ",
	"╰──────────╯ ",
};
#define KG_LOGO_LINES ((int)(sizeof(kg_logo) / sizeof(kg_logo[0])))
#define KG_LOGO_COLS 32

/* Bytes a frame buffer starts out holding.  One frame is hundreds of
 * appends -- ab_fill() alone splits a blank line into 256-byte chunks --
 * so the first allocation may as well cover a small screen outright. */
#define KG_ABUF_MIN_CAPACITY 4096

/* Make `ab` able to hold `need` bytes, doubling.  Sets the sticky oom
 * flag and returns 0 when it cannot, leaving the existing bytes intact. */
static int ab_reserve(struct abuf *ab, int need)
{
	int newcap;
	char *grown;

	if (need <= ab->capacity) {
		return 1;
	}
	newcap = ab->capacity > 0 ? ab->capacity : KG_ABUF_MIN_CAPACITY;
	while (newcap < need) {
		if (newcap > INT_MAX / 2) {
			newcap = need;
			break;
		}
		newcap *= 2;
	}
	KG_PERF_INC(KG_PERF_AB_GROW);
	KG_PERF_ADD(KG_PERF_AB_COPIED, ab->len);
	grown = realloc(ab->b, (size_t)newcap);
	if (grown == NULL) {
		ab->oom = 1;
		return 0;
	}
	ab->b = grown;
	ab->capacity = newcap;
	return 1;
}

int ab_append(struct abuf *ab, const char *s, int len)
{
	if (ab->oom) {
		return 0;
	}
	if (len <= 0) {
		return 1;
	}
	if (ab->len > INT_MAX - len) {
		ab->oom = 1;
		return 0;
	}
	KG_PERF_INC(KG_PERF_AB_APPEND);
	KG_PERF_ADD(KG_PERF_AB_BYTES, len);
	if (!ab_reserve(ab, ab->len + len)) {
		return 0;
	}
	memcpy(ab->b + ab->len, s, len);
	ab->len += len;
	return 1;
}

/* Drop the buffer and every field describing it.  Leaving len, capacity
 * or oom behind over a freed pointer is what a reused abuf would then
 * append through. */
void ab_free(struct abuf *ab)
{
	free(ab->b);
	ab->b = NULL;
	ab->len = 0;
	ab->capacity = 0;
	ab->oom = 0;
}

/* Append a VT100 "move to absolute position" escape (1-based). */
static void ab_move_to(struct abuf *ab, int row, int col)
{
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
	ab_append(ab, buf, len);
}

/* Append n copies of character c to ab in bulk. */
static void ab_fill(struct abuf *ab, char c, int n)
{
	char buf[256];
	while (n > 0) {
		int chunk = n < (int)sizeof(buf) ? n : (int)sizeof(buf);
		memset(buf, c, chunk);
		ab_append(ab, buf, chunk);
		n -= chunk;
	}
}

static void ab_append_spaces(struct abuf *ab, int n) { ab_fill(ab, ' ', n); }

/* Where a run of text came from.  Every context is escaped the same way
 * today; the enum exists so a later exception has to be named rather
 * than smuggled in as a bare ab_append(). */
enum display_text_context {
	DISPLAY_BUFFER_TEXT,
	DISPLAY_FILENAME,
	DISPLAY_STATUS_TEXT,
};

/* Append `len` bytes of untrusted text spelled the way display_glyph_at()
 * says they must be drawn, so no byte of a file, a filename, a directory
 * entry, a Lisp string or a subprocess' output can become terminal
 * syntax.  Returns 0 once the buffer has run out of memory, as
 * ab_append() does. */
static int ab_append_terminal_text(struct abuf *ab, const char *text, int len,
    enum display_text_context context)
{
	int i = 0;

	(void)context;
	while (i < len) {
		struct display_glyph g;

		display_glyph_at(text, len, i, &g);
		if (!ab_append(ab, g.bytes, g.len)) {
			return 0;
		}
		i += g.span;
	}
	return 1;
}

/* Bytes of `text` whose drawn spelling fits in `budget` cells, cut on a
 * glyph boundary so a character is shown whole or not at all and an
 * escape spelling is never halved.  *used takes the cells they occupy. */
static int display_fit(const char *text, int len, int budget, int *used)
{
	int i = 0, cols = 0;

	while (i < len) {
		struct display_glyph g;

		display_glyph_at(text, len, i, &g);
		if (g.width > 0 && cols + g.width > budget) {
			break;
		}
		cols += g.width;
		i += g.span;
	}
	*used = cols;
	return i;
}

/* Render the text rows of one window into ab.
 * win_y, win_x, win_h, win_w describe the window's position/size.
 * rowoff/coloff/numrows/rows describe the buffer viewport.
 * is_active: the window currently has the user's focus; only this one
 * shows the visual-mark region overlay.
 * is_full_width: if true we can use \x1b[0K (erase to EOL) to clear the
 * rest of each row; if false (vertical split) we must space-pad to stay
 * within the window's column range. */
static void draw_window_rows(struct abuf *ab, int win_y, int win_x, int win_h,
    int win_w, int rowoff, int coloff, int numrows, erow *rows, int is_active,
    int is_full_width, int visual_line_mode)
{
	int y, j;
	int region_active = 0;
	int region_s_row = 0, region_s_col = 0;
	int region_e_row = 0, region_e_col = 0;

	if (is_active && bcur()->mark_highlight && bcur()->mark_set) {
		int p_row = wcur()->rowoff + wcur()->cy;
		int p_col = wcur()->coloff + wcur()->cx;
		int m_row = bcur()->mark_row;
		int m_col = bcur()->mark_col;
		if (bcur()->rect_mode) {
			/* Rectangle bounds live in VISUAL-column space.  Each
			 * row in range maps that visual range back to its own
			 * byte / render range — matches what the kill/clear
			 * operations cut, and stays rectangular across rows
			 * with different tab and UTF-8 content. */
			int p_vcol = (p_row < numrows)
			    ? editor_visual_col(&rows[p_row], p_col)
			    : p_col;
			int m_vcol = (m_row < numrows)
			    ? editor_visual_col(&rows[m_row], m_col)
			    : m_col;
			region_s_row = (p_row < m_row) ? p_row : m_row;
			region_e_row = (p_row > m_row) ? p_row : m_row;
			region_s_col = (p_vcol < m_vcol) ? p_vcol : m_vcol;
			region_e_col = (p_vcol > m_vcol) ? p_vcol : m_vcol;
		} else if (m_row < p_row || (m_row == p_row && m_col < p_col)) {
			region_s_row = m_row;
			region_s_col = m_col;
			region_e_row = p_row;
			region_e_col = p_col;
		} else {
			region_s_row = p_row;
			region_s_col = p_col;
			region_e_row = m_row;
			region_e_col = m_col;
		}
		region_active = (region_s_row != region_e_row
		    || region_s_col != region_e_col);
	}

	for (y = 0; y < win_h; y++) {
		int fr;
		int offset;
		if (visual_line_mode) {
			find_visual_row(
			    rows, numrows, win_w, rowoff, y, &fr, &offset);
		} else {
			fr = rowoff + y;
			/* `offset` slices row->render, but coloff is a chars
			 * offset (filecol == coloff + cx everywhere else), so
			 * it has to be converted.  Reading it as a render
			 * offset drew a window that did not contain point on
			 * any horizontally scrolled row holding a tab. */
			offset = fr < numrows
			    ? chars_to_render_col(&rows[fr], coloff)
			    : 0;
		}
		int current_color = -1;
		int current_reverse = 0;
		int hi_lo = -1,
		    hi_hi = -1; /* highlight bounds in render-col, half-open */
		int len, vcol_used = 0;

		ab_move_to(ab, win_y + y, win_x);

		if (fr >= numrows) {
			int filled = 0;
			if (numrows == 0) {
				int start = (win_h - KG_LOGO_LINES) / 2;
				int line = y - start;

				if (line >= 0 && line < KG_LOGO_LINES) {
					const char *str = kg_logo[line];
					int padding
					    = (win_w - KG_LOGO_COLS) / 2;
					int slen = (int)strlen(str);
					int budget;
					int k = 0, vcols = 0;

					if (padding < 0) {
						padding = 0;
					}
					if (padding > 0) {
						ab_append(ab,
						    KG_SHOW_TILDE ? "~" : " ",
						    1);
						filled++;
						padding--;
					}
					while (padding-- > 0) {
						ab_append(ab, " ", 1);
						filled++;
					}
					/* Glyph-aware clip so wider slogan rows
					 * on a narrow or vertically-split pane
					 * don't overflow the window's right
					 * edge into the next pane. */
					budget = win_w - filled;
					if (budget < 0) {
						budget = 0;
					}
					while (k < slen) {
						int w = utf8_width_at(
						    str, slen, k);

						if (w > 0
						    && vcols + w > budget) {
							break;
						}
						vcols += w;
						k++;
					}
					ab_append(ab, str, k);
					filled += vcols;
				}
			}
			if (filled == 0 && KG_SHOW_TILDE) {
				ab_append(ab, "~", 1);
				filled = 1;
			}
			if (is_full_width) {
				ab_append(ab, "\x1b[0K", 4);
			} else {
				ab_append_spaces(ab, win_w - filled);
			}
			continue;
		}

		{
			erow *r = &rows[fr];
			int span = 1;

			KG_ASSERT_RENDER_OFF(r, offset);

			/* Walk render bytes from offset to compute len bounded
			 * by win_w VISIBLE columns, keeping UTF-8 glyphs whole.
			 * Charging each glyph its display width lets a
			 * 79-visual-col line (200+ bytes of box drawing) render
			 * correctly on an 80-col terminal, and stops before a
			 * double-width glyph that would straddle the right
			 * edge — the padding below leaves that cell blank
			 * rather than letting half a glyph bleed into the next
			 * pane. */
			len = 0;
			while (offset + len < r->rsize) {
				int w = utf8_width_at(
				    r->render, r->rsize, offset + len);

				if (w > 0 && vcol_used + w > win_w) {
					break;
				}
				vcol_used += w;
				len++;
			}

			if (region_active && fr >= region_s_row
			    && fr <= region_e_row) {
				if (bcur()->rect_mode) {
					int byte_lo
					    = editor_chars_col_at_visual(
						r, region_s_col);
					int byte_hi
					    = editor_chars_col_at_visual(
						r, region_e_col);
					if (byte_lo > r->size) {
						byte_lo = r->size;
					}
					if (byte_hi > r->size) {
						byte_hi = r->size;
					}
					hi_lo = chars_to_render_col(r, byte_lo);
					hi_hi = chars_to_render_col(r, byte_hi);
				} else {
					hi_lo = (fr == region_s_row)
					    ? chars_to_render_col(
						  r, region_s_col)
					    : 0;
					hi_hi = (fr == region_e_row)
					    ? chars_to_render_col(
						  r, region_e_col)
					    : r->rsize;
				}
			}

			/* One glyph per iteration: a multi-byte character is
			 * emitted whole, so neither an attribute escape nor
			 * the right-edge clip can ever split it, and a byte
			 * that is not a character at all gets the visible
			 * spelling display_glyph_at() gives it.  The whole
			 * row is in scope, not just the visible slice, so a
			 * `coloff` that landed inside a character draws the
			 * same nothing for its bytes that the width loop
			 * above charged for them. */
			for (j = offset; j < offset + len; j += span) {
				int want_rev = (j >= hi_lo && j < hi_hi);
				struct display_glyph g;

				display_glyph_at(r->render, r->rsize, j, &g);
				span = g.span;
				if (want_rev != current_reverse) {
					if (want_rev) {
						ab_append(ab, "\x1b[7m", 4);
					} else {
						ab_append(ab, "\x1b[27m", 5);
					}
					current_reverse = want_rev;
				}
				if (r->hl[j] == HL_NORMAL
				    || r->hl[j] == HL_NONPRINT) {
					if (current_color != -1) {
						ab_append(ab, "\x1b[39m", 5);
						current_color = -1;
					}
				} else {
					int color
					    = editor_syntax_to_color(r->hl[j]);
					if (color != current_color) {
						char cbuf[16];
						int clen = snprintf(cbuf,
						    sizeof(cbuf), "\x1b[%dm",
						    color);
						current_color = color;
						ab_append(ab, cbuf, clen);
					}
				}
				ab_append(ab, g.bytes, g.len);
			}
			/* When rect mode's right edge is past this row's
			 * visual content, extend the highlight into virtual
			 * space with reverse-video spaces — so a rectangle
			 * pulled out past short rows still looks rectangular.
			 */
			if (bcur()->rect_mode && region_active
			    && fr >= region_s_row && fr <= region_e_row) {
				int row_vwidth = editor_visual_col(r, r->size);

				if (region_e_col > row_vwidth) {
					int virt_e = region_e_col - row_vwidth;
					int virt_s = region_s_col > row_vwidth
					    ? region_s_col - row_vwidth
					    : 0;
					int skip = virt_s;
					int rev = virt_e - virt_s;

					if (skip > 0) {
						if (current_reverse) {
							ab_append(
							    ab, "\x1b[27m", 5);
							current_reverse = 0;
						}
						ab_append_spaces(ab, skip);
					}
					if (rev > 0) {
						if (!current_reverse) {
							ab_append(
							    ab, "\x1b[7m", 4);
							current_reverse = 1;
						}
						ab_append_spaces(ab, rev);
					}
				}
			}
			if (current_reverse) {
				ab_append(ab, "\x1b[27m", 5);
			}
		}
		ab_append(ab, "\x1b[39m", 5);
		if (is_full_width) {
			ab_append(ab, "\x1b[0K", 4);
		} else {
			ab_append_spaces(ab, win_w - vcol_used);
		}
	}
}

/* Render the mode line for one window at terminal row ml_row, starting at
 * terminal column win_x (1-based).  Needed for vertical splits where two mode
 * lines share the same terminal row. */
static void draw_mode_line(struct abuf *ab, int ml_row, int win_x, int win_w,
    int bufidx, int is_active, int cur_row, int cur_col, int total_rows,
    int rowoff, int win_h)
{
	char status[512];
	char bname[128];
	int len, used = 0;
	struct editor_buffer *b = &buflist[bufidx];
	const char *modename = b->syntax ? b->syntax->name : "Fundamental";
	const char *changed = "";
	int dirty = b->dirty;
	char pos[8];

	/* Show only the basename in the mode line (Emacs-style); the directory
	 * part is still available via C-x C-b.  buf_display_name() also
	 * prepends the parent directory when another open buffer shares the
	 * basename, so foo and dir/foo can be told apart. */
	buf_display_name(bufidx, bname, sizeof(bname));
	if (b->disk_changed) {
		changed = " (changed)";
	}

	/* Emacs-style position indicator. */
	if (total_rows <= win_h) {
		snprintf(pos, sizeof(pos), "All");
	} else if (rowoff == 0) {
		snprintf(pos, sizeof(pos), "Top");
	} else if (rowoff + win_h >= total_rows) {
		snprintf(pos, sizeof(pos), "Bot");
	} else {
		snprintf(pos, sizeof(pos), "%d%%", rowoff * 100 / total_rows);
	}

	ab_move_to(ab, ml_row, win_x);
	ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2m",
	    4); /* active: reverse; inactive: dim */

	char mode_buf[128];
	int readonly = b->readonly;
	int vline = b->visual_line_mode;
	int ovwrt = b->overwrite_mode;

	snprintf(mode_buf, sizeof(mode_buf), "%s%s%s%s", modename,
	    vline ? " VLine" : "", ovwrt ? " Ovwrt" : "",
	    readonly ? " RO" : "");

	len = snprintf(status, sizeof(status), "%s  %s%s  %s (%d,%d)  (%s)",
	    dirty ? "-**-" : "----", bname, changed, pos, cur_row, cur_col,
	    mode_buf);

	/* snprintf reports what it WOULD have written, so a name long
	 * enough to overflow status[] would otherwise hand ab_append() a
	 * length that reads past the array. */
	if (len >= (int)sizeof(status)) {
		len = (int)sizeof(status) - 1;
	}
	/* The name is the buffer's, so the whole line is untrusted text and
	 * has to be budgeted by drawn cells rather than by bytes: an escape
	 * spelling is wider than the byte it stands for. */
	len = display_fit(status, len, win_w, &used);
	ab_append_terminal_text(ab, status, len, DISPLAY_FILENAME);
	ab_append_spaces(ab, win_w - used);
	ab_append(ab, "\x1b[0m", 4);
}

/* This function writes the whole screen using VT100 escape characters. */
void editor_refresh_screen(void)
{
	struct abuf ab = ABUF_INIT;
	int i, cx;
	int msglen;

	KG_PERF_INC(KG_PERF_REFRESH);
	if (bcur()->visual_line_mode) {
		struct editor_window *w_act = &winlist[win_current];
		int filerow = wcur()->rowoff + wcur()->cy;
		int filecol = wcur()->coloff + wcur()->cx;
		int cursor_vrow = get_visual_row(
		    bcur()->row, bcur()->numrows, w_act->w, filerow, filecol);
		if (cursor_vrow < wcur()->rowoff_visual) {
			wcur()->rowoff_visual = cursor_vrow;
		} else if (cursor_vrow >= wcur()->rowoff_visual + w_act->h) {
			wcur()->rowoff_visual = cursor_vrow - w_act->h + 1;
		}
		wcur()->cx += wcur()->coloff;
		wcur()->coloff = 0;
	}

	ab_append(&ab, "\x1b[?25l", 6); /* Hide cursor. */

	/* ---- Render each window ---- */
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct editor_window *w = &winlist[i];
		int bidx, numrows, rowoff, coloff;
		erow *rows;
		int is_active = (i == win_current);
		int is_full_width = (w->w == win_total_cols);
		int ml_row;
		struct editor_buffer *b;

		if (!w->active) {
			continue;
		}

		/* A window whose handle no longer resolves is drawn as
		 * nothing rather than indexed with: the slot it names may
		 * now hold somebody else's text.  win_check_handles() is
		 * what puts such a window back on a buffer. */
		b = win_buffer(w);
		bidx = win_buffer_slot(w);
		if (!b) {
			continue;
		}

		int vline = b->visual_line_mode;
		/* The rows come from the buffer the window shows, whichever
		 * window this is: there is one row array per buffer now, so
		 * there is no longer a live copy to prefer over a stale one. */
		numrows = b->numrows;
		rows = b->row;
		/* And the scroll comes from the window, whichever window this
		 * is: the selected one is not a special case any more. */
		rowoff = vline ? w->rowoff_visual : w->rowoff;
		coloff = w->coloff;

		draw_window_rows(&ab, w->y, w->x, w->h, w->w, rowoff, coloff,
		    numrows, rows, is_active, is_full_width, vline);

		ml_row = w->y + w->h;
		{
			int wrowoff = vline ? w->rowoff_visual : w->rowoff;
			/* cx/coloff: cx is relative to the horizontal scroll
			 * offset, so the file column is their sum. */
			int filerow = w->rowoff + w->cy;
			int filecol = w->coloff + w->cx;
			int cur_row;
			if (vline) {
				cur_row = get_visual_row(rows, numrows, w->w,
					      filerow, filecol)
				    + 1;
			} else {
				cur_row = filerow + 1;
			}
			int cur_col = editor_display_col(
			    rows, numrows, filerow, filecol);
			int total_rows = vline
			    ? get_total_visual_rows(b->row, b->numrows, w->w)
			    : b->numrows;
			draw_mode_line(&ab, ml_row, w->x, w->w, bidx, is_active,
			    cur_row, cur_col, total_rows, wrowoff, w->h);
		}

		/* Draw vertical separator to the right of non-rightmost
		 * windows. */
		if (w->x + w->w < win_total_cols) {
			int sep_col = w->x + w->w;
			int row;
			for (row = w->y; row < ml_row; row++) {
				ab_move_to(&ab, row, sep_col);
				ab_append(&ab, "\xe2\x94\x82", 3); /* │ */
			}
			/* Mode line row: invert to blend with the mode line. */
			ab_move_to(&ab, ml_row, sep_col);
			ab_append(&ab, "\x1b[7m\xe2\x94\x82\x1b[0m", 11);
		}
	}

	/* ---- Echo area (one row at the very bottom) ---- */
	ab_move_to(&ab, win_total_rows, 1);
	ab_append(&ab, "\x1b[0K", 4);
	msglen = strlen(editor.statusmsg);
	if (msglen && time(NULL) - editor.statusmsg_time < 5) {
		/* Messages interpolate filenames, directory entries, Lisp
		 * strings and subprocess output, so the text is escaped and
		 * the only styling is the renderer's own: a byte range the
		 * caller marked, not bytes the caller spelled.  Cap by drawn
		 * width, not byte count, so a glyph is whole or absent — the
		 * same budget shell_output_fits_echo() measures against. */
		int visible = 0;
		int fit = display_fit(
		    editor.statusmsg, msglen, win_total_cols, &visible);
		int s = editor.statusmsg_emph_start;
		int e = editor.statusmsg_emph_end;

		if (s > fit) {
			s = fit;
		}
		if (e > fit) {
			e = fit;
		}
		ab_append_terminal_text(
		    &ab, editor.statusmsg, s, DISPLAY_STATUS_TEXT);
		if (e > s) {
			ab_append(&ab, "\x1b[1m", 4);
			ab_append_terminal_text(&ab, editor.statusmsg + s,
			    e - s, DISPLAY_STATUS_TEXT);
			ab_append(&ab, "\x1b[22m", 5);
		}
		ab_append_terminal_text(
		    &ab, editor.statusmsg + e, fit - e, DISPLAY_STATUS_TEXT);
		ab_append(&ab, "\x1b[0m", 4); /* close any open attribute */
	}

	/* ---- Place cursor ---- */
	if (editor.echo_cursor_col > 0) {
		/* Minibuffer prompt active: park the cursor on the echo area so
		 * the user can see what they're typing. */
		int col = editor.echo_cursor_col;
		if (col > win_total_cols) {
			col = win_total_cols;
		}
		ab_move_to(&ab, win_total_rows, col);
	} else {
		struct editor_window *w = &winlist[win_current];
		erow *row = (wcur()->rowoff + wcur()->cy < bcur()->numrows)
		    ? &bcur()->row[wcur()->rowoff + wcur()->cy]
		    : NULL;

		cx = 1;
		if (row) {
			/* wcur()->cx is a byte offset into row->chars, but the
			 * cursor must be placed at the visible column: the
			 * cells drawn between the left edge of the window
			 * (chars offset coloff) and point.  editor_visual_col()
			 * is the one model of that -- tab stops measured from
			 * the start of the line, every other glyph worth its
			 * display width, virtual space past EOL one cell per
			 * byte -- and it is what the row above was rendered
			 * with. */
			cx += editor_visual_col(
				  row, wcur()->cx + wcur()->coloff)
			    - editor_visual_col(row, wcur()->coloff);
		}
		if (bcur()->visual_line_mode) {
			int filerow = wcur()->rowoff + wcur()->cy;
			int filecol = wcur()->coloff + wcur()->cx;
			int win_w = w->w > 0 ? w->w : 1;
			int rcol = row
			    ? visual_line_cursor_col(row, filecol, win_w)
			    : 0;
			int cursor_vrow = get_visual_row(bcur()->row,
			    bcur()->numrows, win_w, filerow, filecol);
			int screen_y = cursor_vrow - wcur()->rowoff_visual;
			cx = (rcol % win_w) + 1;
			ab_move_to(&ab, w->y + screen_y, w->x + cx - 1);
		} else {
			ab_move_to(&ab, w->y + wcur()->cy, w->x + cx - 1);
		}
	}

	ab_append(&ab, "\x1b[?25h", 6); /* Show cursor. */
	if (ab.oom) {
		ab_free(&ab);
		fprintf(stderr, "Out of memory\n");
		/* editor_at_exit() is already registered by enable_raw_mode();
		 * exit() runs it before editor_cleanup() because atexit
		 * handlers are LIFO. */
		exit(1);
	}
	if (tty_write(ab.b, ab.len) == -1) {
		/* The terminal is gone.  There is nowhere to report that --
		 * a status message would need the frame that just failed --
		 * so stop the main loop and let the ordinary exit path run. */
		running = 0;
	}
	ab_free(&ab);
}

/* Set an editor status message for the echo area at the bottom. */
void editor_set_status_message(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	vsnprintf(editor.statusmsg, sizeof(editor.statusmsg), fmt, ap);
	va_end(ap);
	editor.statusmsg_time = time(NULL);
	editor.statusmsg_emph_start = 0;
	editor.statusmsg_emph_end = 0;
}

/* Draw `len` bytes of the current message from byte `start` emphasised.
 * Call after editor_set_status_message(), which clears any previous
 * range; a negative or empty range means none. */
void editor_set_status_emphasis(int start, int len)
{
	if (start < 0 || len <= 0) {
		editor.statusmsg_emph_start = 0;
		editor.statusmsg_emph_end = 0;
		return;
	}
	editor.statusmsg_emph_start = start;
	editor.statusmsg_emph_end = start + len;
}
