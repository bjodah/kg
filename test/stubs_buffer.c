/* stubs_buffer.c — globals and stubs for test_buffer that links fileio.o and
 * bufmgr.o. */

#include "../src/def.h"
#include <stdarg.h>
#include <string.h>

/* Globals normally defined in main.c */
struct editor_config editor;
int running = 1;
int suppress_undo = 0;

/* The window globals and the win_* entry points live in stubs_win.c: this
 * file is also linked by test_winmgr, which brings the real winmgr.o. */

/* Global from help.c */
const char *kg_help_lines[] = { NULL };

/* Global normally defined in bufmgr.c but used globally */
int global_auto_revert = 0;

/* No-op stubs */
void editor_set_status_message(const char *fmt, ...) { (void)fmt; }
void editor_set_status_emphasis(int start, int len)
{
	(void)start;
	(void)len;
}
void editor_refresh_screen(void) { }
int editor_read_key(int fd)
{
	(void)fd;
	return 0;
}
int editor_read_raw_byte(int fd)
{
	(void)fd;
	return 0;
}
int editor_read_utf8_seq(int fd, int lead, char *seq)
{
	(void)fd;
	(void)lead;
	(void)seq;
	return 0;
}

/* Reached from dired.o (test_dired), which links no terminal or cursor
 * code: its unit tests exercise the pure row/name parsers only. */
void editor_move_cursor(int key) { (void)key; }
int editor_confirm_yn(int fd, const char *fmt, ...)
{
	(void)fd;
	(void)fmt;
	return 0;
}

/* Not a no-op: buf_apply_local_settings() declares its four local_settings
 * on the stack and relies on the initialiser to clear them, so a stub that
 * returns without writing leaves the "is it set?" flags reading uninitialised
 * memory. */
void local_settings_init(struct local_settings *settings)
{
	memset(settings, 0, sizeof(*settings));
}
int dirlocals_find(
    const char *visited_filename, char *result, size_t result_size)
{
	(void)visited_filename;
	(void)result;
	(void)result_size;
	return 0;
}
int dirlocals_parse(
    const char *source, size_t source_len, struct local_settings *out)
{
	(void)source;
	(void)source_len;
	(void)out;
	return 0;
}
int localvars_parse_modeline(
    const erow *rows, int row_count, struct local_settings *out)
{
	(void)rows;
	(void)row_count;
	(void)out;
	return 0;
}
int localvars_parse_footer(
    const erow *rows, int row_count, struct local_settings *out)
{
	(void)rows;
	(void)row_count;
	(void)out;
	return 0;
}
void local_settings_merge(
    struct local_settings *dest, const struct local_settings *src)
{
	(void)dest;
	(void)src;
}

void editor_path_split(
    const char *path, char *dir, int dsize, char *file, int fsize)
{
	(void)path;
	(void)dir;
	(void)dsize;
	(void)file;
	(void)fsize;
}
int editor_path_complete_entries(const char *dir, const char *prefix,
    struct path_entry *entries, int max, char *lcp, int lcp_size)
{
	(void)dir;
	(void)prefix;
	(void)entries;
	(void)max;
	(void)lcp;
	(void)lcp_size;
	return 0;
}
void editor_path_expand_tilde(char *buf, int bufsize)
{
	(void)buf;
	(void)bufsize;
}
void editor_goto_line_direct(int line, int col)
{
	(void)line;
	(void)col;
}
int editor_picker_match_rank(const char *haystack, const char *needle)
{
	(void)haystack;
	(void)needle;
	return 0;
}

void __attribute__((weak)) editor_cleanup(void) { }
void kg_lisp_shutdown(void) { }
