/* tty.c - Low level terminal handling */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "compile.h"
#include "def.h"
#include "keyevent.h"
#include "process_table.h"

static struct termios orig_termios; /* In order to restore at exit.*/
static unsigned char *pending_input;
static size_t pending_input_len;
static size_t pending_input_off;
static size_t pending_input_cap;
static volatile sig_atomic_t pending_resize;

/* One key handed back to the reader, delivered before anything else and
 * never recorded into a keyboard macro again -- it was recorded when it
 * was first read.  A single slot is enough: the only producer is
 * editor_read_utf8_seq(), which over-reads at most one byte and returns
 * immediately, and every consumer drains the slot before another read
 * can reach the producer. */
static int unread_key_slot = -1;

static void unread_key(int key) { unread_key_slot = key; }

/* ESC followed by one of these bytes is that Meta combination: the base
 * a plain, unmodified press of the same byte would carry, plus whatever
 * modifier on top of Meta the two Ctrl-letter rows need.  Named bases
 * (KEY_BASE_DEL, KEY_BASE_RET) are for the terminal's own idea of
 * Backspace and Enter, which is a raw byte, not the "DEL"/"RET" key
 * kg's own decoder never emits from a bare press.  Field order is base
 * first (not the byte-first order the row initializers below read in):
 * int32_t base first keeps the struct's padding to what its alignment
 * needs, instead of the 6 stray bytes a leading char costs it. */
struct meta_key {
	int32_t base;
	char byte;
	uint8_t extra_mods;
};

static const struct meta_key meta_keys[] = {
	{ 'f', 'f', 0 },
	{ 'b', 'b', 0 },
	{ 'd', 'd', 0 },
	{ 'g', 'g', 0 },
	{ 'h', 'h', 0 },
	{ 'v', 'v', 0 },
	{ 'w', 'w', 0 },
	{ 'q', 'q', 0 },
	{ 't', 't', 0 },
	{ 'y', 'y', 0 },
	{ KEY_BASE_DEL, '\x7f', 0 },
	{ KEY_BASE_DEL, '\b', 0 },
	{ '%', '%', 0 },
	{ '@', '@', 0 },
	{ ';', ';', 0 },
	{ ':', ':', 0 },
	{ 'x', 'x', 0 },
	{ '^', '^', 0 },
	{ 'u', 'u', 0 },
	{ 'l', 'l', 0 },
	{ 'c', 'c', 0 },
	{ '!', '!', 0 },
	{ '|', '|', 0 },
	{ '<', '<', 0 },
	{ '>', '>', 0 },
	{ '{', '{', 0 },
	{ '}', '}', 0 },
	{ 'm', 'm', 0 },
	{ 'a', 'a', 0 },
	{ 'e', 'e', 0 },
	{ 'r', 'r', 0 },
	{ '\\', '\\', 0 },
	{ ' ', ' ', 0 },
	{ 'z', 'z', 0 },
	{ 'p', 'p', 0 },
	{ 'n', 'n', 0 },
	/* M-- is Emacs' negative-argument.  Without this row ESC '-' fell
	 * through to the multi-byte branch below and read a *second* byte --
	 * the ESC of whatever key followed -- so `M-- M-x` swallowed the
	 * M-x.  07D added M-- to the argument accumulator but nothing ever
	 * produced the event for it to accumulate. */
	{ '-', '-', 0 },
	{ KEY_BASE_RET, '\r', 0 },
	{ KEY_BASE_RET, '\n', 0 },
	{ 's', '\x13', KEY_MOD_CTRL }, /* ESC C-s */
	{ 'r', '\x12', KEY_MOD_CTRL }, /* ESC C-r */
};

/* The key_event a Meta-prefixed byte stands for, or a zero base when
 * `byte` starts no Meta combination kg knows. */
static struct key_event lookup_meta_key(char byte)
{
	size_t i;

	for (i = 0; i < sizeof(meta_keys) / sizeof(meta_keys[0]); i++) {
		if (meta_keys[i].byte == byte) {
			return (struct key_event) { meta_keys[i].base,
				(uint8_t)(KEY_MOD_META
				    | meta_keys[i].extra_mods) };
		}
	}
	return (struct key_event) { 0, 0 };
}

void disable_raw_mode(int fd)
{
	free(pending_input);
	pending_input = NULL;
	pending_input_len = 0;
	pending_input_off = 0;
	pending_input_cap = 0;
	unread_key_slot = -1;
#ifdef KG_FUZZ
	(void)fd;
	editor.rawmode = 0;
	return;
#endif
	/* Don't even check the return value as it's too late. */
	if (editor.rawmode) {
		tcsetattr(fd, TCSAFLUSH, &orig_termios);
		editor.rawmode = 0;
	}
}

#ifndef KG_FUZZ
static int reserve_pending_input(void)
{
	unsigned char *new_input;
	size_t new_cap;

	if (pending_input_len < pending_input_cap) {
		return 1;
	}
	if (pending_input_off > 0) {
		memmove(pending_input, pending_input + pending_input_off,
		    pending_input_len - pending_input_off);
		pending_input_len -= pending_input_off;
		pending_input_off = 0;
		if (pending_input_len < pending_input_cap) {
			return 1;
		}
	}
	if (pending_input_cap > SIZE_MAX / 2) {
		return 0;
	}
	new_cap = pending_input_cap ? pending_input_cap * 2 : 64;
	new_input = realloc(pending_input, new_cap);
	if (!new_input) {
		return 0;
	}
	pending_input = new_input;
	pending_input_cap = new_cap;
	return 1;
}
#endif

static int read_input_byte(int fd, unsigned char *byte)
{
	int nread;

	if (fd == STDIN_FILENO && pending_input_off < pending_input_len) {
		*byte = pending_input[pending_input_off++];
		if (pending_input_off == pending_input_len) {
			free(pending_input);
			pending_input = NULL;
			pending_input_len = 0;
			pending_input_off = 0;
			pending_input_cap = 0;
		}
		return 1;
	}
	nread = read(fd, byte, 1);
	return nread;
}

/* More queued input than a burst of keystrokes could produce — i.e. a
 * paste is in flight.  Escape sequences and type-ahead stay well below
 * this, so ordinary typing still gets a redraw per key. */
#define INPUT_FLOOD_THRESHOLD 64

/* Is a paste flooding stdin?  The main loop skips the screen refresh
 * while this holds so a large paste is consumed at insert speed: a full
 * redraw per pasted byte writes megabytes of frames nobody sees, and can
 * block forever against a paster that only starts reading our output
 * after it has finished writing the paste. */
int editor_input_flood(int fd)
{
	int queued = 0;

	if (ioctl(fd, FIONREAD, &queued) == -1) {
		queued = 0;
	}
	if (fd == STDIN_FILENO) {
		queued += (int)(pending_input_len - pending_input_off);
	}
	return queued > INPUT_FLOOD_THRESHOLD;
}

/* Poll for C-g while Fe is evaluating.  Other raw bytes are retained and
 * replayed through the ordinary key decoder after evaluation returns. */
int editor_check_quit_pending(void)
{
#ifdef KG_FUZZ
	return 0;
#else
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	unsigned char byte;
	int checked = 0;

	while (
	    checked++ < 64 && poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
		if (!reserve_pending_input()) {
			return 0;
		}
		if (read(STDIN_FILENO, &byte, 1) != 1) {
			return 0;
		}
		if (byte == CTRL_G) {
			return 1;
		}
		pending_input[pending_input_len++] = byte;
		pfd.revents = 0;
	}
	return 0;
#endif
}

/* Put a whole frame on the terminal.  Returns 0 once every byte is out
 * and -1 when the terminal is gone.  A single write(2) is allowed to
 * stop short -- a signal lands mid-frame, or the pty's buffer fills
 * because whatever is reading us is slow -- and half a frame is a screen
 * that lies about the buffer, so this retries through write_all() rather
 * than discarding the result.  Callers that cannot repaint answer -1 by
 * shutting down; they cannot report it, since reporting needs a frame. */
int tty_write(const void *buf, size_t n)
{
#ifdef KG_FUZZ
	(void)buf;
	(void)n;
	return 0;
#else
	return write_all(STDOUT_FILENO, buf, n) < 0 ? -1 : 0;
#endif
}

/* Called at exit to avoid remaining in raw mode. */
void editor_at_exit(void)
{
#ifdef KG_FUZZ
	return;
#endif
	/* Hand the terminal back the way it was found.  A frame can stop
	 * half-written now that tty_write() reports a short write instead of
	 * discarding it, and editor_refresh_screen() answers by shutting
	 * down -- so the attributes that frame would have closed itself may
	 * still be open: the cursor hidden by the "\x1b[?25l" every frame
	 * starts with, and whatever colour or reverse video the row it died
	 * on had set. */
	(void)tty_write("\x1b[0m", 4); /* Close any open attribute */
	(void)tty_write("\x1b[?25h", 6); /* Show the cursor */
	(void)tty_write("\x1b[2J", 4); /* Clear entire screen */
	(void)tty_write("\x1b[H", 3); /* Move cursor to top-left */

	disable_raw_mode(STDIN_FILENO);
}

/* Raw mode: 1960 magic shit. */
int enable_raw_mode(int fd)
{
#ifdef KG_FUZZ
	(void)fd;
	editor.rawmode = 1;
	return 0;
#else
	struct termios raw;

	if (editor.rawmode) {
		return 0; /* Already enabled. */
	}
	if (!isatty(STDIN_FILENO)) {
		goto fatal;
	}
	atexit(editor_at_exit);
	if (tcgetattr(fd, &orig_termios) == -1) {
		goto fatal;
	}

	raw = orig_termios; /* modify the original mode */
	/* input modes: no break, no CR to NL, no parity check, no strip char,
	 * no start/stop output control. */
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	/* output modes - disable post processing */
	raw.c_oflag &= ~(OPOST);
	/* control modes - set 8 bit chars */
	raw.c_cflag |= (CS8);
	/* local modes - choing off, canonical off, no extended functions,
	 * no signal chars (^Z,^C) */
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	/* control chars - set return condition: min number of bytes and timer.
	 */
	raw.c_cc[VMIN] = 0; /* Return each byte, or zero for timeout. */
	raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

	/* put terminal in raw mode after flushing */
	if (tcsetattr(fd, TCSAFLUSH, &raw) < 0) {
		goto fatal;
	}
	editor.rawmode = 1;
	return 0;

fatal:
	errno = ENOTTY;
	return -1;
#endif
}

/* Bare ESC: nothing more arrived, or what did arrive names nothing kg
 * decodes.  A named base with no modifier, same as a deliberate press
 * of the Escape key. */
static struct key_event bare_esc(void)
{
	return (struct key_event) { KEY_BASE_ESC, 0 };
}

/* Decode an escape sequence (ESC byte already consumed) into a key_event. */
static struct key_event parse_escape(int fd)
{
	unsigned char seq[6];
	struct key_event event;

	if (read_input_byte(fd, seq) != 1) {
		return bare_esc();
	}

	/* Meta+key: ESC followed by a single character */
	event = lookup_meta_key((char)seq[0]);
	if (event.base) {
		return event;
	}
	if (seq[0] >= '0' && seq[0] <= '9') {
		return (
		    struct key_event) { '0' + (seq[0] - '0'), KEY_MOD_META };
	}

	if (read_input_byte(fd, seq + 1) != 1) {
		return bare_esc();
	}

	/* ESC [ sequences */
	if (seq[0] == '[') {
		if (seq[1] >= '0' && seq[1] <= '9') {
			if (read_input_byte(fd, seq + 2) != 1) {
				return bare_esc();
			}
			if (seq[2] == '~') {
				switch (seq[1]) {
				case '1':
				case '7':
					return (struct key_event) {
						KEY_BASE_HOME, 0
					};
				case '2':
					return (struct key_event) {
						KEY_BASE_INSERT, 0
					};
				case '3':
					return (struct key_event) {
						KEY_BASE_DELETE, 0
					};
				case '4':
				case '8':
					return (struct key_event) {
						KEY_BASE_END, 0
					};
				case '5':
					return (struct key_event) {
						KEY_BASE_PRIOR, 0
					};
				case '6':
					return (struct key_event) {
						KEY_BASE_NEXT, 0
					};
				}
			} else if (seq[2] >= '0' && seq[2] <= '9') {
				/* Two-digit: ESC[<d1><d2>~ (F3=ESC[13~,
				 * F4=ESC[14~) */
				if (read_input_byte(fd, seq + 3) != 1) {
					return bare_esc();
				}
				if (seq[3] == '~' && seq[1] == '1') {
					switch (seq[2]) {
					case '3':
						return (struct key_event) {
							KEY_BASE_F3, 0
						};
					case '4':
						return (struct key_event) {
							KEY_BASE_F4, 0
						};
					}
				}
			} else if (seq[2] == ';') {
				/* ESC [ 1 ; N x  modified-key, N=2 Shift, N=5
				 * Ctrl */
				if (read_input_byte(fd, seq + 3) != 1) {
					return bare_esc();
				}
				if (read_input_byte(fd, seq + 4) != 1) {
					return bare_esc();
				}
				if (seq[1] == '1' && seq[3] == '5') {
					switch (seq[4]) {
					case 'A':
						return (struct key_event) {
							KEY_BASE_UP,
							KEY_MOD_CTRL
						};
					case 'B':
						return (struct key_event) {
							KEY_BASE_DOWN,
							KEY_MOD_CTRL
						};
					case 'C':
						return (struct key_event) {
							KEY_BASE_RIGHT,
							KEY_MOD_CTRL
						};
					case 'D':
						return (struct key_event) {
							KEY_BASE_LEFT,
							KEY_MOD_CTRL
						};
					case 'H':
						return (struct key_event) {
							KEY_BASE_HOME,
							KEY_MOD_CTRL
						};
					case 'F':
						return (struct key_event) {
							KEY_BASE_END,
							KEY_MOD_CTRL
						};
					}
				} else if (seq[1] == '1' && seq[3] == '2') {
					switch (seq[4]) {
					case 'A':
						return (struct key_event) {
							KEY_BASE_UP,
							KEY_MOD_SHIFT
						};
					case 'B':
						return (struct key_event) {
							KEY_BASE_DOWN,
							KEY_MOD_SHIFT
						};
					case 'C':
						return (struct key_event) {
							KEY_BASE_RIGHT,
							KEY_MOD_SHIFT
						};
					case 'D':
						return (struct key_event) {
							KEY_BASE_LEFT,
							KEY_MOD_SHIFT
						};
					case 'H':
						return (struct key_event) {
							KEY_BASE_HOME,
							KEY_MOD_SHIFT
						};
					case 'F':
						return (struct key_event) {
							KEY_BASE_END,
							KEY_MOD_SHIFT
						};
					}
				} else if (seq[4] == '~') {
					/* ESC [ N ; M ~  modified Insert/Delete
					 * (CUA clipboard) */
					if (seq[1] == '2' && seq[3] == '2') {
						return (struct key_event) {
							KEY_BASE_INSERT,
							KEY_MOD_SHIFT
						};
					}
					if (seq[1] == '2' && seq[3] == '5') {
						return (struct key_event) {
							KEY_BASE_INSERT,
							KEY_MOD_CTRL
						};
					}
					if (seq[1] == '3' && seq[3] == '2') {
						return (struct key_event) {
							KEY_BASE_DELETE,
							KEY_MOD_SHIFT
						};
					}
				}
			}
		} else {
			switch (seq[1]) {
			case 'A':
				return (struct key_event) { KEY_BASE_UP, 0 };
			case 'B':
				return (struct key_event) { KEY_BASE_DOWN, 0 };
			case 'C':
				return (struct key_event) { KEY_BASE_RIGHT, 0 };
			case 'D':
				return (struct key_event) { KEY_BASE_LEFT, 0 };
			case 'H':
				return (struct key_event) { KEY_BASE_HOME, 0 };
			case 'F':
				return (struct key_event) { KEY_BASE_END, 0 };
			}
		}
		/* ESC O sequences */
	} else if (seq[0] == 'O') {
		switch (seq[1]) {
		case 'H':
			return (struct key_event) { KEY_BASE_HOME, 0 };
		case 'F':
			return (struct key_event) { KEY_BASE_END, 0 };
		case 'R':
			return (struct key_event) { KEY_BASE_F3, 0 };
		case 'S':
			return (struct key_event) { KEY_BASE_F4, 0 };
		}
	}
	return bare_esc();
}

/* Block until one input byte arrives, servicing signals and the idle
 * polls meanwhile.  `idle` selects the main-loop poll set (auto-revert
 * plus pending compilation restarts) over the plain one used by
 * minibuffer prompts.  Returns the byte, or -1 once the input stream is
 * gone and the editor is shutting down. */
static int read_key_byte(int fd, int idle)
{
	unsigned char c;
	int nread;

	while (1) {
		nread = read_input_byte(fd, &c);
		if (nread == 0) {
			if (idle) {
				int changed = 0;
				changed |= autorevert_poll();
				changed |= compilation_poll();
				compilation_start_pending_restart();
				/* Not part of `changed`: this only moves
				 * bytes fd -> queue and publishes events, so
				 * there is nothing here yet for a repaint to
				 * show -- the drain that would change the
				 * screen runs from a safe point, never from
				 * this idle loop. */
				kg_process_table_poll();
				if (changed) {
					editor_refresh_screen();
				}
			} else {
				compilation_poll();
			}
			continue;
		}
		if (nread == -1) {
			if (errno == EINTR) {
				if (editor_process_pending_signals() == 1) {
					editor_refresh_screen();
				}
				continue;
			}
			running = 0;
			return -1;
		}
		return c;
	}
}

/* Body shared by the three readers below: replay macro keys, read one
 * byte, optionally decode it (a bare byte through key_event_from_byte(),
 * an escape sequence through parse_escape()), and record the result for
 * a macro in progress.
 *
 * `decode_escapes` off is editor_read_raw_byte()'s case: the event
 * handed back is the byte itself, base equal to the byte and no
 * modifier, because nothing downstream wants it decoded -- quoted-insert
 * inserts exactly the byte typed, and a UTF-8 continuation byte is not a
 * key at all.  A byte recovered from the unread slot or a macro in
 * replay is a byte that was itself read by one of the three readers, so
 * it is decoded (or not) the same way here as it would have been the
 * first time; the macro queue holds whichever shape its reader already
 * produced, and is returned as-is. */
static struct key_event read_key_common(int fd, int idle, int decode_escapes)
{
	struct key_event event;
	int c;

	if (unread_key_slot >= 0) {
		int raw = unread_key_slot;

		unread_key_slot = -1;
		return decode_escapes ? key_event_from_byte(raw)
				      : (struct key_event) { raw, 0 };
	}
	if (macro_next_key(&event)) {
		return event;
	}

	c = read_key_byte(fd, idle);
	if (c < 0) {
		return (struct key_event) { 0, 0 };
	}

	if (decode_escapes && c == ESC) {
		event = parse_escape(fd);
	} else if (decode_escapes) {
		event = key_event_from_byte(c);
	} else {
		event = (struct key_event) { c, 0 };
	}
	macro_on_key(event);
	return event;
}

/* Read a key from the terminal in raw mode, decoding escape sequences.
 * During macro replay returns pre-recorded keys; during recording saves
 * every key so the full sequence (including sub-prompt characters) is
 * captured in one place. */
struct key_event editor_read_key(int fd) { return read_key_common(fd, 0, 1); }

/* Read a single byte from the terminal without decoding escape sequences.
 * Used by quoted-insert so that an ESC, an arrow-key prefix, or any other
 * byte the user is trying to embed literally ends up in the buffer as
 * itself rather than being interpreted as the start of a meta key. */
int editor_read_raw_byte(int fd) { return (int)read_key_common(fd, 0, 0).base; }

/* Collect the UTF-8 sequence a terminal sends for one multi-byte
 * character: `lead` is the byte already read, the continuation bytes are
 * read here.  `seq` must hold 4 bytes.  Returns the sequence length, or 0
 * when `lead` starts nothing (ASCII or a byte no sequence begins with) or
 * the promised continuation bytes never arrived — callers drop the input
 * rather than store half a character. */
int editor_read_utf8_seq(int fd, int lead, char *seq)
{
	int need, i;

	if (lead > 0xFF) {
		return 0; /* one of the >0xFF soft key codes */
	}
	need = utf8_lead_extra((unsigned char)lead);
	if (need == 0) {
		return 0; /* ASCII, or a byte no sequence starts with */
	}
	seq[0] = (char)lead;
	for (i = 1; i <= need; i++) {
		int b = editor_read_raw_byte(fd);

		if (!running) {
			return 0;
		}
		if (!utf8_is_cont((unsigned char)b)) {
			/* The promised continuation never came, so the lead
			 * is dropped -- but the byte that ended the sequence
			 * is a keystroke in its own right and used to be
			 * swallowed with it: typing a corrupt lead and then
			 * "A" lost the A.  Hand it back to the reader. */
			unread_key(b);
			return 0;
		}
		seq[i] = (char)b;
	}
	return need + 1;
}

/* Top-level main-loop variant of editor_read_key: while waiting for the
 * next key, run the auto-revert poll on every 100 ms read timeout so
 * external file changes are noticed without requiring a keystroke.
 * Minibuffer prompts and y/n confirmations call the plain editor_read_key
 * instead so they aren't redrawn (or silently reverted) under the user. */
struct key_event editor_read_key_idle(int fd)
{
	return read_key_common(fd, 1, 1);
}

/* One numeric field of a terminal report: at least one decimal digit and
 * nothing else.  The running value stops growing well below INT_MAX, so
 * a long digit run saturates instead of overflowing; what it saturates
 * to is far past any real terminal, and kg_normalize_window_size()
 * throws it out. */
static int parse_report_field(const char **p, int *out)
{
	const char *s = *p;
	int v = 0;

	if (*s < '0' || *s > '9') {
		return -1;
	}
	while (*s >= '0' && *s <= '9') {
		if (v <= KG_MAX_ROWS) {
			v = v * 10 + (*s - '0');
		}
		s++;
	}
	*p = s;
	*out = v;
	return 0;
}

/* Parse a DSR cursor-position report, exactly "ESC [ <rows> ; <cols> R":
 * decimal digits only in both fields, no sign, no other parameter bytes,
 * nothing after the R.  Returns 0 with *rows and *cols set, else -1.
 *
 * sscanf("%d;%d") accepted a leading '-', ignored whatever followed the
 * numbers, and had undefined behaviour on a digit run that overflows
 * int.  The reply comes from the terminal, which is not a trusted source
 * of any of that: a stray paste can forge one. */
int kg_parse_cursor_report(const char *buf, int *rows, int *cols)
{
	const char *p = buf;

	if (p[0] != ESC || p[1] != '[') {
		return -1;
	}
	p += 2;
	if (parse_report_field(&p, rows) != 0 || *p++ != ';') {
		return -1;
	}
	if (parse_report_field(&p, cols) != 0 || *p++ != 'R') {
		return -1;
	}
	return *p == '\0' ? 0 : -1;
}

/* 1 with a usable size in the two out-parameters, 0 when the probe
 * result is not a plausible terminal size at all.
 *
 * Below the minimum the size is clamped up rather than refused: clamping
 * keeps win_reflow()'s row total consistent and needs no separate
 * "terminal too small" render path.  Above the maximum it is refused --
 * the frame buffer's arithmetic is int (display.c), so rows * cols has
 * to stay far from INT_MAX, and a terminal that large does not exist. */
int kg_normalize_window_size(int rows, int cols, int *out_rows, int *out_cols)
{
	if (rows <= 0 || cols <= 0 || rows > KG_MAX_ROWS
	    || cols > KG_MAX_COLS) {
		return 0;
	}
	*out_rows = rows < KG_MIN_ROWS ? KG_MIN_ROWS : rows;
	*out_cols = cols < KG_MIN_COLS ? KG_MIN_COLS : cols;
	return 1;
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor is stored at *rows and *cols and 0 is returned. */
int get_cursor_position(int ifd, int ofd, int *rows, int *cols)
{
#ifdef KG_FUZZ
	(void)ifd;
	(void)ofd;
	*rows = win_total_rows ? win_total_rows : 24;
	*cols = win_total_cols ? win_total_cols : 80;
	return 0;
#else
	unsigned int i = 0;
	char buf[32];

	if (!isatty(ifd) || !isatty(ofd)) {
		return -1;
	}

	/* Report cursor location */
	if (write(ofd, "\x1b[6n", 4) != 4) {
		return -1;
	}

	/* Read the response: ESC [ rows ; cols R */
	while (i < sizeof(buf) - 1) {
		ssize_t nread = read(ifd, buf + i, 1);
		if (nread == -1) {
			if (errno == EINTR) {
				if (editor_process_pending_signals() == 1) {
					editor_refresh_screen();
				}
				continue;
			}
			break;
		}
		if (nread == 0) {
			break;
		}
		i++;
		if (buf[i - 1] == 'R') {
			break;
		}
	}
	buf[i] = '\0';

	return kg_parse_cursor_report(buf, rows, cols);
#endif
}

/* Try to get the number of columns in the current terminal. If the ioctl()
 * call fails the function will try to query the terminal itself.
 * Returns 0 on success, -1 on error. */
int get_window_size(int ifd, int ofd, int *rows, int *cols)
{
#ifdef KG_FUZZ
	(void)ifd;
	(void)ofd;
	*rows = 24;
	*cols = 80;
	return 0;
#else
	struct winsize ws;

	if (ioctl(ofd, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0
	    || ws.ws_row == 0) {
		/* ioctl() failed. Try to query the terminal itself. */
		int orig_row, orig_col, retval;

		/* Get the initial position so we can restore it later. */
		retval = get_cursor_position(ifd, ofd, &orig_row, &orig_col);
		if (retval == -1) {
			goto failed;
		}

		/* Go to right/bottom margin and get position. */
		if (write(ofd, "\x1b[999C\x1b[999B", 12) != 12) {
			goto failed;
		}
		retval = get_cursor_position(ifd, ofd, rows, cols);
		if (retval == -1) {
			goto failed;
		}

		/* Restore position. */
		char seq[32];
		snprintf(seq, 32, "\x1b[%d;%dH", orig_row, orig_col);
		if (write(ofd, seq, strlen(seq)) == -1) {
			/* Can't recover... */
		}
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
	}
	/* Neither source is trusted: the ioctl reports whatever the kernel
	 * was last told, and the DSR reply comes off the wire.  An
	 * implausible answer is an error, so the caller's retry loop gets
	 * another go rather than laying out to it. */
	if (!kg_normalize_window_size(*rows, *cols, rows, cols)) {
		goto failed;
	}
	return 0;

failed:
	return -1;
#endif
}

/* Adopt a new terminal size: the windows reflow to it, or, when
 * win_init() has not run yet, the single viewport takes it directly
 * (two rows go to the status and echo lines). */
static void apply_window_size(int rows, int cols)
{
	/* Last gate before the layout: whatever route the numbers took to
	 * get here, nothing below this line sees a zero, a negative or an
	 * absurd dimension. */
	if (!kg_normalize_window_size(rows, cols, &rows, &cols)) {
		return;
	}
	win_total_rows = rows;
	win_total_cols = cols;
	if (win_count > 0) {
		win_reflow();
	} else {
		wcur()->h = rows - 2;
		wcur()->w = cols;
	}
}

/* Probe terminal dimensions using ANSI escape sequences, bypassing ioctl.
 * Necessary on serial consoles where SIGWINCH is never delivered and
 * TIOCGWINSZ may return stale host-side values.
 * Saves and restores cursor position around the probe. */
void probe_window_size(void)
{
#ifdef KG_FUZZ
	win_total_rows = 24;
	win_total_cols = 80;
	wcur()->h = 22;
	wcur()->w = 80;
	return;
#else
	int new_rows, new_cols, orig_row, orig_col;
	char seq[32];

	if (get_cursor_position(
		STDIN_FILENO, STDOUT_FILENO, &orig_row, &orig_col)
	    == -1) {
		return;
	}

	/* Drive cursor to the bottom-right corner, then read back position. */
	if (write(STDOUT_FILENO, "\x1b[999B\x1b[999C", 12) != 12) {
		goto restore;
	}
	if (get_cursor_position(
		STDIN_FILENO, STDOUT_FILENO, &new_rows, &new_cols)
	    == -1) {
		goto restore;
	}

	if (new_rows != win_total_rows || new_cols != win_total_cols) {
		apply_window_size(new_rows, new_cols);
	}

restore:
	snprintf(seq, sizeof(seq), "\x1b[%d;%dH", orig_row, orig_col);
	(void)tty_write(seq, strlen(seq));
#endif
}

void update_window_size(void)
{
#ifdef KG_FUZZ
	apply_window_size(24, 80);
	return;
#else
	const int max_attempts = 3;
	int new_rows, new_cols;
	int attempts = 0;

	/* Try to get window size with retry logic */
	while (attempts < max_attempts) {
		if (get_window_size(
			STDIN_FILENO, STDOUT_FILENO, &new_rows, &new_cols)
		    == 0) {
			apply_window_size(new_rows, new_cols);
			return;
		}

		attempts++;
		if (attempts < max_attempts) {
			/* Brief delay before retry (10ms) */
			usleep(10000);
		}
	}

	/* If all attempts failed, keep current dimensions and warn user */
	editor_set_status_message("Warning: failed updating window size");
#endif
}

void handle_sig_winch(int unused __attribute__((unused)))
{
#ifdef KG_FUZZ
	(void)unused;
	update_window_size();
	return;
#else
	int saved_errno = errno;
	pending_resize = 1;
	errno = saved_errno;
#endif
}

int editor_process_pending_signals(void)
{
	if (pending_resize) {
		pending_resize = 0;
		update_window_size();
		return 1;
	}
	return 0;
}

/* Suspend the editor (C-z): restore terminal, stop the process, then
 * re-enable raw mode and redraw when resumed with fg. */
void editor_suspend(void)
{
#ifdef KG_FUZZ
	return;
#else
	disable_raw_mode(STDIN_FILENO);
	(void)tty_write("\x1b[2J\x1b[H", 7); /* clear screen, cursor home */
	raise(SIGTSTP);
	/* Execution resumes here when the shell sends SIGCONT (fg). */
	enable_raw_mode(STDIN_FILENO);
	update_window_size();
	editor_refresh_screen();
#endif
}
