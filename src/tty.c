/* tty.c - Low level terminal handling */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "def.h"

static struct termios orig_termios; /* In order to restore at exit.*/
static unsigned char *pending_input;
static size_t pending_input_len;
static size_t pending_input_off;
static size_t pending_input_cap;

struct key_map {
	char byte;
	int key;
};

static const struct key_map alt_keys[] = {
	{ 'f', ALT_F },
	{ 'b', ALT_B },
	{ 'd', ALT_D },
	{ 'g', ALT_G },
	{ 'h', ALT_H },
	{ 'v', ALT_V },
	{ 'w', ALT_W },
	{ 'q', ALT_Q },
	{ '\x7f', ALT_BACKSPACE },
	{ '\b', ALT_BACKSPACE },
	{ '%', ALT_PCT },
	{ ';', ALT_SEMICOLON },
	{ ':', ALT_COLON },
	{ 'x', ALT_X },
	{ '^', ALT_CARET },
	{ 'u', ALT_U },
	{ 'l', ALT_L },
	{ 'c', ALT_C },
	{ '!', ALT_BANG },
	{ '|', ALT_PIPE },
	{ '<', ALT_LT },
	{ '>', ALT_GT },
	{ '{', ALT_LBRACE },
	{ '}', ALT_RBRACE },
	{ 'm', ALT_M },
	{ 'a', ALT_A },
	{ 'e', ALT_E },
	{ 'r', ALT_R },
	{ '\\', ALT_BACKSLASH },
	{ ' ', ALT_SPACE },
	{ 'z', ALT_Z },
};

static int lookup_alt_key(char byte)
{
	size_t i;

	for (i = 0; i < sizeof(alt_keys) / sizeof(alt_keys[0]); i++) {
		if (alt_keys[i].byte == byte) {
			return alt_keys[i].key;
		}
	}
	return 0;
}

void disable_raw_mode(int fd)
{
	free(pending_input);
	pending_input = NULL;
	pending_input_len = 0;
	pending_input_off = 0;
	pending_input_cap = 0;
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

/* Called at exit to avoid remaining in raw mode. */
void editor_at_exit(void)
{
#ifdef KG_FUZZ
	return;
#endif
	/* Clear screen and reset cursor position before exiting. */
	tty_write("\x1b[2J", 4); /* Clear entire screen */
	tty_write("\x1b[H", 3); /* Move cursor to top-left */

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

/* Decode an escape sequence (ESC byte already consumed) into a key code. */
static int parse_escape(int fd)
{
	unsigned char seq[6];
	int key;

	if (read_input_byte(fd, seq) != 1) {
		return ESC; /* bare ESC */
	}

	/* Alt+key: ESC followed by a single character */
	key = lookup_alt_key(seq[0]);
	if (key) {
		return key;
	}
	if (seq[0] >= '0' && seq[0] <= '9') {
		return ALT_0 + (seq[0] - '0');
	}

	if (read_input_byte(fd, seq + 1) != 1) {
		return ESC;
	}

	/* ESC [ sequences */
	if (seq[0] == '[') {
		if (seq[1] >= '0' && seq[1] <= '9') {
			if (read_input_byte(fd, seq + 2) != 1) {
				return ESC;
			}
			if (seq[2] == '~') {
				switch (seq[1]) {
				case '3':
					return DEL_KEY;
				case '5':
					return PAGE_UP;
				case '6':
					return PAGE_DOWN;
				}
			} else if (seq[2] >= '0' && seq[2] <= '9') {
				/* Two-digit: ESC[<d1><d2>~ (F3=ESC[13~,
				 * F4=ESC[14~) */
				if (read_input_byte(fd, seq + 3) != 1) {
					return ESC;
				}
				if (seq[3] == '~' && seq[1] == '1') {
					switch (seq[2]) {
					case '3':
						return KEY_F3;
					case '4':
						return KEY_F4;
					}
				}
			} else if (seq[2] == ';') {
				/* ESC [ 1 ; N x  modified-key, N=2 Shift, N=5
				 * Ctrl */
				if (read_input_byte(fd, seq + 3) != 1) {
					return ESC;
				}
				if (read_input_byte(fd, seq + 4) != 1) {
					return ESC;
				}
				if (seq[1] == '1' && seq[3] == '5') {
					switch (seq[4]) {
					case 'A':
						return CTRL_ARROW_UP;
					case 'B':
						return CTRL_ARROW_DOWN;
					case 'C':
						return CTRL_ARROW_RIGHT;
					case 'D':
						return CTRL_ARROW_LEFT;
					case 'H':
						return CTRL_HOME;
					case 'F':
						return CTRL_END;
					}
				} else if (seq[1] == '1' && seq[3] == '2') {
					switch (seq[4]) {
					case 'A':
						return SHIFT_ARROW_UP;
					case 'B':
						return SHIFT_ARROW_DOWN;
					case 'C':
						return SHIFT_ARROW_RIGHT;
					case 'D':
						return SHIFT_ARROW_LEFT;
					case 'H':
						return SHIFT_HOME;
					case 'F':
						return SHIFT_END;
					}
				} else if (seq[4] == '~') {
					/* ESC [ N ; M ~  modified Insert/Delete
					 * (CUA clipboard) */
					if (seq[1] == '2' && seq[3] == '2') {
						return SHIFT_INSERT;
					}
					if (seq[1] == '2' && seq[3] == '5') {
						return CTRL_INSERT;
					}
					if (seq[1] == '3' && seq[3] == '2') {
						return SHIFT_DELETE;
					}
				}
			}
		} else {
			switch (seq[1]) {
			case 'A':
				return ARROW_UP;
			case 'B':
				return ARROW_DOWN;
			case 'C':
				return ARROW_RIGHT;
			case 'D':
				return ARROW_LEFT;
			case 'H':
				return HOME_KEY;
			case 'F':
				return END_KEY;
			}
		}
		/* ESC O sequences */
	} else if (seq[0] == 'O') {
		switch (seq[1]) {
		case 'H':
			return HOME_KEY;
		case 'F':
			return END_KEY;
		case 'R':
			return KEY_F3;
		case 'S':
			return KEY_F4;
		}
	}
	return ESC;
}

/* Read a key from the terminal in raw mode, decoding escape sequences.
 * During macro replay returns pre-recorded keys; during recording saves
 * every key so the full sequence (including sub-prompt characters) is
 * captured in one place. */
int editor_read_key(int fd)
{
	unsigned char c;
	int nread;
	int key;

	key = macro_next_key();
	if (key >= 0) {
		return key;
	}

	while ((nread = read_input_byte(fd, &c)) == 0)
		;
	if (nread == -1) {
		running = 0;
		return 0;
	}

	key = (c == ESC) ? parse_escape(fd) : (unsigned char)c;
	macro_on_key(key);
	return key;
}

/* Read a single byte from the terminal without decoding escape sequences.
 * Used by quoted-insert so that an ESC, an arrow-key prefix, or any other
 * byte the user is trying to embed literally ends up in the buffer as
 * itself rather than being interpreted as the start of a meta key. */
int editor_read_raw_byte(int fd)
{
	unsigned char c;
	int nread;
	int key;

	key = macro_next_key();
	if (key >= 0) {
		return key;
	}

	while ((nread = read_input_byte(fd, &c)) == 0)
		;
	if (nread == -1) {
		running = 0;
		return 0;
	}

	key = (unsigned char)c;
	macro_on_key(key);
	return key;
}

/* Top-level main-loop variant of editor_read_key: while waiting for the
 * next key, run the auto-revert poll on every 100 ms read timeout so
 * external file changes are noticed without requiring a keystroke.
 * Minibuffer prompts and y/n confirmations call the plain editor_read_key
 * instead so they aren't redrawn (or silently reverted) under the user. */
int editor_read_key_idle(int fd)
{
	unsigned char c;
	int nread;
	int key;

	key = macro_next_key();
	if (key >= 0) {
		return key;
	}

	while ((nread = read_input_byte(fd, &c)) == 0) {
		if (autorevert_poll()) {
			editor_refresh_screen();
		}
	}
	if (nread == -1) {
		running = 0;
		return 0;
	}

	key = (c == ESC) ? parse_escape(fd) : (unsigned char)c;
	macro_on_key(key);
	return key;
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

	/* Report cursor location */
	if (write(ofd, "\x1b[6n", 4) != 4) {
		return -1;
	}

	/* Read the response: ESC [ rows ; cols R */
	while (i < sizeof(buf) - 1) {
		if (read(ifd, buf + i, 1) != 1) {
			break;
		}
		if (buf[i] == 'R') {
			break;
		}
		i++;
	}
	buf[i] = '\0';

	/* Parse it. */
	if (buf[0] != ESC || buf[1] != '[') {
		return -1;
	}
	if (sscanf(buf + 2, "%d;%d", rows, cols) != 2) {
		return -1;
	}

	return 0;
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

	if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
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
		return 0;
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}

failed:
	return -1;
#endif
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
	editor.screenrows = 22;
	editor.screencols = 80;
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
		win_total_rows = new_rows;
		win_total_cols = new_cols;
		if (win_count > 0) {
			win_reflow();
		} else {
			editor.screenrows = new_rows - 2;
			editor.screencols = new_cols;
		}
	}

restore:
	snprintf(seq, sizeof(seq), "\x1b[%d;%dH", orig_row, orig_col);
	tty_write(seq, strlen(seq));
#endif
}

void update_window_size(void)
{
#ifdef KG_FUZZ
	win_total_rows = 24;
	win_total_cols = 80;
	if (win_count > 0) {
		win_reflow();
	} else {
		editor.screenrows = 22;
		editor.screencols = 80;
	}
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
			win_total_rows = new_rows;
			win_total_cols = new_cols;
			if (win_count > 0) {
				win_reflow();
			} else {
				/* win_init() not yet called; set a sensible
				 * default. */
				editor.screenrows = new_rows - 2;
				editor.screencols = new_cols;
			}
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
	update_window_size(); /* calls win_reflow() which clamps cursors */
	editor_refresh_screen();
#endif
}

/* Suspend the editor (C-z): restore terminal, stop the process, then
 * re-enable raw mode and redraw when resumed with fg. */
void editor_suspend(void)
{
#ifdef KG_FUZZ
	return;
#else
	disable_raw_mode(STDIN_FILENO);
	tty_write("\x1b[2J\x1b[H", 7); /* clear screen, cursor home */
	raise(SIGTSTP);
	/* Execution resumes here when the shell sends SIGCONT (fg). */
	enable_raw_mode(STDIN_FILENO);
	update_window_size();
	editor_refresh_screen();
#endif
}
