/* test_tty.c — unit tests for tty signal handling */

#include "../src/def.h"
#include "../src/keyevent.h"
#include "test.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Stubs for tty.c dependencies */
void win_reflow(void) { }
void editor_refresh_screen(void) { }
int autorevert_poll(void) { return 0; }
int macro_next_key(struct key_event *out)
{
	(void)out;
	return 0;
}
void macro_on_key(struct key_event key) { (void)key; }

/* tty_write() writes through write_all(), so this binary links fileio.o
 * for the real loop rather than reimplementing it; these are the prompt
 * and dired entry points fileio.c reaches for and nothing here calls. */
int dired_fill_current(const char *dir)
{
	(void)dir;
	return 0;
}
int editor_confirm_yn(int fd, const char *fmt, ...)
{
	(void)fd;
	(void)fmt;
	return 0;
}
void editor_prompt_prefill_dir(char *buf, int bufsize)
{
	(void)bufsize;
	buf[0] = '\0';
}
enum minibuf_result editor_read_line_path(
    int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	(void)bufsize;
	buf[0] = '\0';
	return MINIBUF_CANCELLED;
}
/* Reached from fileio.o, which flushes a completed save back into the
 * buffer table; this suite has no buffer table. */

static void test_sigwinch_handling(void)
{
	/* pending_resize is static in tty.c, but we can call handle_sig_winch
	 * and verify that editor_process_pending_signals() detects it and
	 * returns 1. */

	/* First, ensure calling it without signal returns 0 */
	CHECK(editor_process_pending_signals() == 0);

	/* Coalesced signals still request one resize, and the handler does not
	 * disturb errno in the interrupted code. */
	errno = EBUSY;
	handle_sig_winch(SIGWINCH);
	CHECK(errno == EBUSY);
	handle_sig_winch(SIGWINCH);

	/* editor_process_pending_signals() should now detect it and return 1 */
	CHECK(editor_process_pending_signals() == 1);

	/* And subsequent calls should return 0 again */
	CHECK(editor_process_pending_signals() == 0);
}

/* The main loop skips redraws while editor_input_flood() reports a
 * paste-sized input queue, so a large paste costs a handful of
 * refreshes instead of one per byte.  A lone queued byte (ordinary
 * typing) must not suppress the redraw. */
static void test_input_flood_detects_paste_sized_queue(void)
{
	int fds[2];
	char burst[128];
	char byte = 'x';

	memset(burst, 'x', sizeof(burst));
	if (pipe(fds) != 0) {
		CHECK(!"pipe failed");
		return;
	}
	CHECK(editor_input_flood(fds[0]) == 0);
	CHECK(write(fds[1], &byte, 1) == 1);
	CHECK(editor_input_flood(fds[0]) == 0);
	CHECK(write(fds[1], burst, sizeof(burst)) == sizeof(burst));
	CHECK(editor_input_flood(fds[0]) == 1);
	close(fds[0]);
	close(fds[1]);
}

/* Mocks for the write syscall behind write_all(), in the shape
 * test_buffer.c's test_write_all uses. */
static int mock_eintr_count;
static int mock_short_count;
static size_t mock_written;

static ssize_t mock_write_retryable(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	if (mock_eintr_count > 0) {
		mock_eintr_count--;
		errno = EINTR;
		return -1;
	}
	if (mock_short_count > 0 && count > 2) {
		mock_short_count--;
		mock_written += 2;
		return 2;
	}
	mock_written += count;
	return (ssize_t)count;
}

static ssize_t mock_write_error(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	(void)count;
	errno = EACCES;
	return -1;
}

static ssize_t mock_write_zero(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	(void)count;
	return 0;
}

static char mock_sink[128];
static size_t mock_sink_len;

static ssize_t mock_write_capture(int fd, const void *buf, size_t count)
{
	(void)fd;
	if (mock_sink_len + count < sizeof(mock_sink)) {
		memcpy(mock_sink + mock_sink_len, buf, count);
		mock_sink_len += count;
	}
	return (ssize_t)count;
}

/* A frame that stops half-written leaves the screen lying about the
 * buffer, so tty_write() reports only "all of it went out" or "the
 * terminal is gone". */
static void test_tty_write_completes_or_reports_loss(void)
{
	const char frame[] = "\x1b[H\x1b[2Jhello";
	size_t len = sizeof(frame) - 1;

	mock_eintr_count = 3;
	mock_short_count = 2;
	mock_written = 0;
	editor_write_fn = mock_write_retryable;
	CHECK(tty_write(frame, len) == 0);
	CHECK(mock_written == len); /* every byte, retries included */
	CHECK(mock_eintr_count == 0);
	CHECK(mock_short_count == 0);

	editor_write_fn = mock_write_error;
	CHECK(tty_write(frame, len) == -1);

	/* A write(2) that reports zero bytes on a non-empty buffer is not
	 * progress; retrying it forever would hang the editor. */
	editor_write_fn = mock_write_zero;
	CHECK(tty_write(frame, len) == -1);

	/* Nothing to write is nothing to fail at. */
	CHECK(tty_write(frame, 0) == 0);

	editor_write_fn = write;
}

/* The frame that shut the editor down may have died between the
 * "hide cursor" every frame starts with and the "show cursor" it ends
 * with, or in the middle of a coloured row.  The exit path is the last
 * chance to hand the terminal back the way it was found. */
static void test_at_exit_hands_the_terminal_back(void)
{
	mock_sink_len = 0;
	editor.rawmode = 0;
	editor.screen_painted = 1;
	editor_write_fn = mock_write_capture;
	editor_at_exit();
	editor_write_fn = write;

	mock_sink[mock_sink_len] = '\0';
	CHECK(strstr(mock_sink, "\x1b[?25h") != NULL); /* cursor visible */
	CHECK(strstr(mock_sink, "\x1b[0m") != NULL); /* attributes closed */
}

/* The other side of the same coin.  Raw mode is entered before the first
 * frame is painted (src/main.c), so kg can exit having drawn nothing --
 * the one startup error path does exactly that, printing to stderr.  A
 * screen kg never wrote on is not kg's to clear, and the "\x1b[2J" would
 * take the message with it. */
static void test_at_exit_leaves_an_unpainted_screen_alone(void)
{
	mock_sink_len = 0;
	editor.rawmode = 0;
	editor.screen_painted = 0;
	editor_write_fn = mock_write_capture;
	editor_at_exit();
	editor_write_fn = write;

	CHECK(mock_sink_len == 0);
}

/* The DSR reply arrives on the wire and is as trustworthy as anything
 * else the terminal sends -- a paste can forge one.  It is accepted only
 * in its exact shape. */
static void test_cursor_report_parses_exact_shape_only(void)
{
	int r = -1, c = -1;

	CHECK(kg_parse_cursor_report("\x1b[24;80R", &r, &c) == 0);
	CHECK(r == 24 && c == 80);
	CHECK(kg_parse_cursor_report("\x1b[1;1R", &r, &c) == 0);
	CHECK(r == 1 && c == 1);

	/* Zero is a valid parse; kg_normalize_window_size() is what rules
	 * on whether a size is usable. */
	CHECK(kg_parse_cursor_report("\x1b[0;0R", &r, &c) == 0);
	CHECK(r == 0 && c == 0);

	/* A sign, a missing field, a missing terminator, trailing bytes,
	 * the wrong introducer, or nothing at all: all refused.  sscanf
	 * accepted the first four. */
	CHECK(kg_parse_cursor_report("\x1b[-1;80R", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("\x1b[;80R", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("\x1b[24;R", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("\x1b[24R", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("\x1b[24;80", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("\x1b[24;80Rjunk", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("\x1b[24; 80R", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("\x1b]24;80R", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("24;80R", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("", &r, &c) == -1);
	CHECK(kg_parse_cursor_report("\x1b", &r, &c) == -1);

	/* A digit run long enough to overflow int saturates instead, which
	 * kg_normalize_window_size() then throws out. */
	CHECK(kg_parse_cursor_report("\x1b[99999999999;80R", &r, &c) == 0);
	CHECK(r > KG_MAX_ROWS && c == 80);
}

/* Layout code never receives a zero, a negative, or an absurd size. */
static void test_window_size_normalises_or_refuses(void)
{
	int r = 0, c = 0;

	CHECK(kg_normalize_window_size(24, 80, &r, &c) == 1);
	CHECK(r == 24 && c == 80);

	/* Too small clamps up: the layout stays consistent and needs no
	 * separate "terminal too small" path. */
	CHECK(kg_normalize_window_size(1, 1, &r, &c) == 1);
	CHECK(r == KG_MIN_ROWS && c == KG_MIN_COLS);
	CHECK(kg_normalize_window_size(2, 5, &r, &c) == 1);
	CHECK(r == KG_MIN_ROWS && c == KG_MIN_COLS);
	CHECK(kg_normalize_window_size(KG_MIN_ROWS, KG_MIN_COLS, &r, &c) == 1);
	CHECK(r == KG_MIN_ROWS && c == KG_MIN_COLS);

	/* Nonsense is refused, so the caller retries or keeps what it had. */
	CHECK(kg_normalize_window_size(0, 80, &r, &c) == 0);
	CHECK(kg_normalize_window_size(24, 0, &r, &c) == 0);
	CHECK(kg_normalize_window_size(-1, 80, &r, &c) == 0);
	CHECK(kg_normalize_window_size(24, -80, &r, &c) == 0);
	CHECK(kg_normalize_window_size(KG_MAX_ROWS + 1, 80, &r, &c) == 0);
	CHECK(kg_normalize_window_size(24, KG_MAX_COLS + 1, &r, &c) == 0);
	CHECK(kg_normalize_window_size(INT_MAX, INT_MAX, &r, &c) == 0);

	/* A refusal leaves the out-parameters alone. */
	CHECK(r == KG_MIN_ROWS && c == KG_MIN_COLS);
}

/* A malformed UTF-8 lead is dropped, but the byte that proved it
 * malformed is the user's next keystroke and used to be dropped with it:
 * typing a corrupt lead and then "A" lost the A.
 *
 * The write end of the pipe stays open throughout: read_key_byte() reads
 * nread == 0 as a VTIME timeout and loops, so an EOF here would hang.
 * Every test writes exactly the bytes it consumes. */
static void test_malformed_utf8_keeps_the_following_key(void)
{
	int fds[2];
	char seq[4];

	if (pipe(fds) != 0) {
		CHECK(!"pipe failed");
		return;
	}
	running = 1;

	/* "E2 41 42": the three-byte lead is promised two continuation
	 * bytes and gets an 'A'.  The 'A' comes back once, and the 'B'
	 * behind it is still queued -- so it came back exactly once. */
	CHECK(write(fds[1], "AB", 2) == 2);
	CHECK(editor_read_utf8_seq(fds[0], 0xE2, seq) == 0);
	CHECK(KEY_IS(editor_read_key(fds[0]), 'A', 0));
	CHECK(KEY_IS(editor_read_key(fds[0]), 'B', 0));

	/* The rescued byte need not be printable. */
	CHECK(write(fds[1], "\x07", 1) == 1);
	CHECK(editor_read_utf8_seq(fds[0], 0xE2, seq) == 0);
	CHECK(KEY_IS(editor_read_key(fds[0]), 'g', KEY_MOD_CTRL));

	/* Two malformed leads back to back: the second lead is itself the
	 * byte rescued from the first. */
	CHECK(write(fds[1], "\xe3\x41", 2) == 2);
	CHECK(editor_read_utf8_seq(fds[0], 0xE2, seq) == 0);
	CHECK(KEY_IS(editor_read_key(fds[0]), 0xE3, 0));
	CHECK(editor_read_utf8_seq(fds[0], 0xE3, seq) == 0);
	CHECK(KEY_IS(editor_read_key(fds[0]), 'A', 0));

	/* A truncated four-byte lead followed by a whole two-byte glyph:
	 * the glyph survives intact. */
	CHECK(write(fds[1], "\xc3\xa9z", 3) == 3);
	CHECK(editor_read_utf8_seq(fds[0], 0xF0, seq) == 0);
	CHECK(KEY_IS(editor_read_key(fds[0]), 0xC3, 0));
	CHECK(editor_read_utf8_seq(fds[0], 0xC3, seq) == 2);
	CHECK(seq[0] == '\xc3' && seq[1] == '\xa9');
	CHECK(KEY_IS(editor_read_key(fds[0]), 'z', 0));

	/* A lone continuation byte starts nothing, so nothing is read and
	 * nothing is owed back. */
	CHECK(editor_read_utf8_seq(fds[0], 0x80, seq) == 0);
	CHECK(write(fds[1], "y", 1) == 1);
	CHECK(KEY_IS(editor_read_key(fds[0]), 'y', 0));

	close(fds[0]);
	close(fds[1]);
}

static struct key_event decode_escape(const char *bytes, size_t len)
{
	struct key_event got = { -1, 0 };
	int fds[2];

	if (pipe(fds) != 0) {
		CHECK(!"pipe failed");
		return got;
	}
	running = 1;
	CHECK(write(fds[1], bytes, len) == (ssize_t)len);
	close(fds[1]);
	got = editor_read_key(fds[0]);
	close(fds[0]);
	return got;
}

static void check_escape(const char *label, const char *bytes, size_t len,
    int32_t base, uint8_t mods)
{
	struct key_event got = decode_escape(bytes, len);

	CHECKF(KEY_IS(got, base, mods),
	    "%s: got base=%d mods=%u, want base=%d mods=%u", label, got.base,
	    got.mods, base, mods);
}

#define CHECK_ESCAPE(bytes_, base_, mods_)                                     \
	check_escape((bytes_), (bytes_), sizeof(bytes_) - 1, (base_), (mods_))

/* Characterizes both the old spellings and the generic CSI decoder's new
 * surface.  Every F-key form is explicit so gaps or a shifted table cannot
 * accidentally pass via a representative sample. */
static void test_escape_sequences_decode_to_key_events(void)
{
	static const struct {
		const char *bytes;
		size_t len;
		struct key_event want;
	} cases[] = {
		{ "\x1b[C", 3, { KEY_BASE_RIGHT, 0 } },
		{ "\x1b[1;5D", 6, { KEY_BASE_LEFT, KEY_MOD_CTRL } },
		{ "\x1b[1;2C", 6, { KEY_BASE_RIGHT, KEY_MOD_SHIFT } },
		{ "\x1b"
		  "f",
		    2, { 'f', KEY_MOD_META } },
		{ "\x1b"
		  "5",
		    2, { '5', KEY_MOD_META } },
		{ "\x1b\x13", 2, { 's', KEY_MOD_CTRL | KEY_MOD_META } },
		{ "\x1b[13~", 5, { KEY_BASE_F3, 0 } },
		{ "\x1b[2~", 4, { KEY_BASE_INSERT, 0 } },
		{ "\x1b[5~", 4, { KEY_BASE_PRIOR, 0 } },
		{ "\x1bOH", 3, { KEY_BASE_HOME, 0 } },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		check_escape(cases[i].bytes, cases[i].bytes, cases[i].len,
		    cases[i].want.base, cases[i].want.mods);
	}

	CHECK_ESCAPE("\x1b", KEY_BASE_ESC, 0); /* lone-ESC timeout/EOF */
	CHECK_ESCAPE("\x1bOP", KEY_BASE_F1, 0);
	CHECK_ESCAPE("\x1bOQ", KEY_BASE_F2, 0);
	CHECK_ESCAPE("\x1bOR", KEY_BASE_F3, 0);
	CHECK_ESCAPE("\x1bOS", KEY_BASE_F4, 0);
	CHECK_ESCAPE("\x1b[11~", KEY_BASE_F1, 0);
	CHECK_ESCAPE("\x1b[12~", KEY_BASE_F2, 0);
	CHECK_ESCAPE("\x1b[13~", KEY_BASE_F3, 0);
	CHECK_ESCAPE("\x1b[14~", KEY_BASE_F4, 0);
	CHECK_ESCAPE("\x1b[15~", KEY_BASE_F5, 0);
	CHECK_ESCAPE("\x1b[15;5~", KEY_BASE_F5, KEY_MOD_CTRL);
	CHECK_ESCAPE("\x1b[17~", KEY_BASE_F6, 0);
	CHECK_ESCAPE("\x1b[18~", KEY_BASE_F7, 0);
	CHECK_ESCAPE("\x1b[19~", KEY_BASE_F8, 0);
	CHECK_ESCAPE("\x1b[20~", KEY_BASE_F9, 0);
	CHECK_ESCAPE("\x1b[21~", KEY_BASE_F10, 0);
	CHECK_ESCAPE("\x1b[23~", KEY_BASE_F11, 0);
	CHECK_ESCAPE("\x1b[24~", KEY_BASE_F12, 0);
	CHECK_ESCAPE("\x1b[1;5P", KEY_BASE_F1, KEY_MOD_CTRL);
}

static void test_every_xterm_modifier_bit_combination(void)
{
	static const uint8_t expected[] = {
		KEY_MOD_SHIFT,
		KEY_MOD_META,
		KEY_MOD_SHIFT | KEY_MOD_META,
		KEY_MOD_CTRL,
		KEY_MOD_SHIFT | KEY_MOD_CTRL,
		KEY_MOD_META | KEY_MOD_CTRL,
		KEY_MOD_SHIFT | KEY_MOD_META | KEY_MOD_CTRL,
	};
	unsigned value;

	for (value = 2; value <= 8; value++) {
		char f5[16], arrow[16], ss3[16], csi_f1[16];

		snprintf(f5, sizeof(f5), "\x1b[15;%u~", value);
		snprintf(arrow, sizeof(arrow), "\x1b[1;%uA", value);
		snprintf(ss3, sizeof(ss3), "\x1bO1;%uP", value);
		snprintf(csi_f1, sizeof(csi_f1), "\x1b[1;%uP", value);
		check_escape(
		    f5, f5, strlen(f5), KEY_BASE_F5, expected[value - 2]);
		check_escape(arrow, arrow, strlen(arrow), KEY_BASE_UP,
		    expected[value - 2]);
		check_escape(
		    ss3, ss3, strlen(ss3), KEY_BASE_F1, expected[value - 2]);
		check_escape(csi_f1, csi_f1, strlen(csi_f1), KEY_BASE_F1,
		    expected[value - 2]);
	}

	/* An omitted first field is the ECMA default of 1 in this form. */
	CHECK_ESCAPE("\x1b[;3D", KEY_BASE_LEFT, KEY_MOD_META);
	/* The old CUA spellings now travel through the same generic path. */
	CHECK_ESCAPE("\x1b[2;2~", KEY_BASE_INSERT, KEY_MOD_SHIFT);
	CHECK_ESCAPE("\x1b[2;5~", KEY_BASE_INSERT, KEY_MOD_CTRL);
	CHECK_ESCAPE("\x1b[3;2~", KEY_BASE_DELETE, KEY_MOD_SHIFT);
}

static void check_invalid_then_key(
    const char *label, const char *bytes, size_t len, int32_t next_base)
{
	struct key_event first, next;
	int fds[2];

	if (pipe(fds) != 0) {
		CHECK(!"pipe failed");
		return;
	}
	running = 1;
	CHECK(write(fds[1], bytes, len) == (ssize_t)len);
	close(fds[1]);
	first = editor_read_key(fds[0]);
	next = editor_read_key(fds[0]);
	CHECKF(KEY_IS(first, KEY_BASE_ESC, 0),
	    "%s: malformed sequence decoded as base=%d mods=%u", label,
	    first.base, first.mods);
	CHECKF(KEY_IS(next, next_base, 0), "%s: next key is base=%d mods=%u",
	    label, next.base, next.mods);
	close(fds[0]);
}

static void test_invalid_escape_sequences_are_bounded(void)
{
	static const char capped[] = "\x1b[00000000000000x";
	static const char *const invalid[] = {
		"\x1b[15;0~", /* explicit modifier values are 2..8 */
		"\x1b[15;1~",
		"\x1b[15;9~",
		"\x1b[15;~", /* no empty modifier */
		"\x1b[;5~", /* tilde form needs a keycode */
		"\x1bO;5S", /* omitted defaults belong to CSI, not SS3 */
		"\x1b[0;5A", /* cursor default is omitted or 1 */
		"\x1b[1A", /* not a keyboard modifier form */
	};
	size_t i;

	for (i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
		CHECKF(KEY_IS(decode_escape(invalid[i], strlen(invalid[i])),
			   KEY_BASE_ESC, 0),
		    "%s was accepted", invalid[i]);
	}
	/* 16 and 22 are holes in the xterm function-key numbering. */
	check_invalid_then_key("F-key gap 16", "\x1b[16~x", 6, 'x');
	check_invalid_then_key("F-key gap 22", "\x1b[22~x", 6, 'x');
	/* An unknown final belongs to the unsupported sequence; only the byte
	 * after it is an ordinary key. */
	check_invalid_then_key("unknown final", "\x1b[1zx", 5, 'x');
	/* CSI and SS3 share collection syntax, not key-code families. */
	check_invalid_then_key("SS3 tilde", "\x1bO15~x", 6, 'x');
	check_invalid_then_key("bare CSI P", "\x1b[Px", 4, 'x');
	check_invalid_then_key("default CSI P", "\x1b[;5Px", 6, 'x');
	/* Once parameter grammar is invalid, consume through its final byte;
	 * only the ordinary key after that complete malformed CSI survives. */
	check_invalid_then_key("colon subparameter", "\x1b[1:2Ax", 7, 'x');
	check_invalid_then_key("private parameter", "\x1b[?1Ax", 6, 'x');
	check_invalid_then_key("intermediate byte", "\x1b[1 Ax", 6, 'x');
	check_invalid_then_key("excess parameter", "\x1b[1;2;3Ax", 9, 'x');
	check_invalid_then_key(
	    "numeric overflow", "\x1b[42949672960~x", 15, 'x');
	/* ESC + '[' + fourteen zeroes exactly fills the fixed 16-byte cap;
	 * the collector must not probe and consume byte 17. */
	check_invalid_then_key(
	    "fixed sequence cap", capped, sizeof(capped) - 1, 'x');
}

static void test_incomplete_escape_keeps_later_input(void)
{
	struct key_event first, next;
	int fds[2], flags;

	if (pipe(fds) != 0) {
		CHECK(!"pipe failed");
		return;
	}
	flags = fcntl(fds[0], F_GETFL);
	CHECK(flags >= 0);
	CHECK(fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) == 0);
	running = 1;
	CHECK(write(fds[1], "\x1b[15", 4) == 4);
	first = editor_read_key(fds[0]);
	CHECK(KEY_IS(first, KEY_BASE_ESC, 0));
	CHECK(write(fds[1], "x", 1) == 1);
	next = editor_read_key(fds[0]);
	CHECK(KEY_IS(next, 'x', 0));
	close(fds[0]);
	close(fds[1]);
}

int main(void)
{
	RUN(test_sigwinch_handling);
	RUN(test_input_flood_detects_paste_sized_queue);
	RUN(test_tty_write_completes_or_reports_loss);
	RUN(test_at_exit_hands_the_terminal_back);
	RUN(test_at_exit_leaves_an_unpainted_screen_alone);
	RUN(test_cursor_report_parses_exact_shape_only);
	RUN(test_window_size_normalises_or_refuses);
	RUN(test_malformed_utf8_keeps_the_following_key);
	RUN(test_escape_sequences_decode_to_key_events);
	RUN(test_every_xterm_modifier_bit_combination);
	RUN(test_invalid_escape_sequences_are_bounded);
	RUN(test_incomplete_escape_keeps_later_input);
	return test_summary();
}
