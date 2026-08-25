/* stubs_buffer.c — globals and stubs for test_buffer that links fileio.o and
 * bufmgr.o. */

#include "../src/def.h"
#include <stdarg.h>
#include <string.h>

/* Globals normally defined in main.c */
struct editor_config editor;
int running = 1;
/* main.c latches this from Lisp; a stub never inhibits the
 * startup screen, so display.c draws it exactly as it always did. */
int inhibit_startup_screen = 0;

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
void key_reset_pending_sequence(void) { }

/* A scripted keystroke queue, so a test can drive the minibuffer and
 * picker read loops (buf_read_name(), editor_read_line_path()) that
 * otherwise sit in editor_read_key() forever.  test_key_script is set by
 * the test; when it runs out, C-g is returned so a loop always terminates
 * rather than hanging the suite. */
const struct key_event *test_key_script;
int test_key_script_len;
int test_key_script_pos;

struct key_event editor_read_key(int fd)
{
	(void)fd;
	if (test_key_script != NULL
	    && test_key_script_pos < test_key_script_len) {
		return test_key_script[test_key_script_pos++];
	}
	return (struct key_event) { 'g', KEY_MOD_CTRL };
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
/* -1, not 0: 0 means "found, and `result` names the file", and a stub that
 * says so without writing `result` sends the caller to open() a path made of
 * stack garbage. */
int dirlocals_find(
    const char *visited_filename, char *result, size_t result_size)
{
	(void)visited_filename;
	(void)result;
	(void)result_size;
	return -1;
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

/* The real split's outputs must always be NUL-terminated; a stub that
 * writes nothing hands path_prompt_redraw() an uninitialized buffer,
 * which valgrind and MSan reject (and plain builds pass by stack
 * luck). Keep the contract, skip the tilde/opendir behaviour. */
void editor_path_split(
    const char *path, char *dir, int dsize, char *file, int fsize)
{
	snprintf(dir, dsize, "./");
	snprintf(file, fsize, "%s", path);
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
int editor_path_expand_tilde(char *buf, int bufsize)
{
	(void)buf;
	(void)bufsize;
	return 0;
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
int editor_picker_filter(picker_name_fn name_at, void *data, const char *query,
    const char **names, int *order, int max, int *prefix_matches)
{
	(void)name_at;
	(void)data;
	(void)query;
	(void)names;
	(void)order;
	(void)max;
	*prefix_matches = 0;
	return 0;
}

void __attribute__((weak)) editor_cleanup(void) { }
void kg_lisp_shutdown(void) { }

/* bufmgr's kill path calls this; the buffer binary has no evaluator to
 * run a hook in, and nothing here is about Lisp. */
void kg_lisp_run_kill_buffer_hook(struct kg_buffer_handle handle)
{
	(void)handle;
}

void kg_bracketed_paste_record(char *data, size_t len)
{
	(void)data;
	(void)len;
}
void kg_bracketed_paste_clear(void) { }
const char *kg_bracketed_paste_data(size_t *len)
{
	if (len) {
		*len = 0;
	}
	return NULL;
}
