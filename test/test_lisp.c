/* test_lisp.c - Fe interpreter lifecycle regression tests */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/event.h"
#include "../src/keybind.h"
#include "../src/keymap.h"
#include "../src/lisp.h"
#include "../src/process_table.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char test_status_message[512];
extern char test_command_name[128];
extern int test_command_calls;
/* stubs_noyank.c's cmd_invoke() reaches Lisp commands through these,
 * because it is also linked into binaries with no Lisp objects at all.
 * Installed by main() below, which is what makes (command-execute
 * 'lisp-cmd) a real nested activation in these tests rather than the
 * CMD_UNKNOWN the stub answered before. */
extern int (*test_lisp_command_exists)(const char *name);
extern int (*test_lisp_command_run)(const char *name, int fd);
extern int test_run_command_with_prefix(
    const char *name, struct command_prefix prefix);

/* word.o is linked for forward-word/backward-word, and basic.o for the
 * window-cycle commands; their interactive primitives drag in terminal
 * entry points the Lisp tests never reach. */
void editor_refresh_screen(void) { }
void probe_window_size(void) { }
int tty_write(const void *buf, size_t n)
{
	(void)buf;
	(void)n;
	return 0;
}

int editor_read_raw_byte(int fd)
{
	(void)fd;
	return 0;
}

static void setup_editor(void)
{
	free_all_rows();
	undo_free();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	winlist[0].active = 1;
	wcur()->buf = buf_handle(buf_current);
	bcur()->filename = "/tmp/bridge.txt";
	undo_init();
	test_status_message[0] = '\0';
	test_command_name[0] = '\0';
	test_command_calls = 0;
}

static void teardown_editor(void)
{
	free_all_rows();
	bcur()->row = nullptr;
	bcur()->numrows = 0;
	undo_free();
}

static void test_disabled(void)
{
	char result[128] = "";

	CHECK(kg_lisp_init() != 0);
	CHECK(kg_lisp_eval_string("(+ 1 2)", 7, result, sizeof(result)) != 0);
	CHECK(strstr(result, "not compiled in") != nullptr);
	CHECK(kg_lisp_load_file("unused.el") != 0);
	CHECK(strstr(kg_lisp_last_error(), "not compiled in") != nullptr);
	CHECK(kg_lisp_load_init() == 0);
	kg_lisp_set_interrupt_check(nullptr);
	kg_lisp_shutdown();
	kg_lisp_shutdown();
}

static void test_lifecycle(void)
{
	char result[128] = "";

	kg_lisp_shutdown();
	CHECK(kg_lisp_eval_string("1", 1, result, sizeof(result)) != 0);
	CHECK(strstr(result, "not initialized") != nullptr);
	CHECK(strstr(kg_lisp_last_error(), "not initialized") != nullptr);
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_init() == 0);
	kg_lisp_shutdown();
	kg_lisp_shutdown();
}

static void test_sized_input(void)
{
	char result[128] = "";
	static const char embedded_nul[] = { '1', '\0', '2' };
	static const char truncated[] = "(+ 1 2) ignored";

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string(
		  embedded_nul, sizeof(embedded_nul), result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "eval:") != nullptr);
	CHECK(strstr(result, "embedded NUL byte") != nullptr);
	CHECK(kg_lisp_eval_string(truncated, 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "3") == 0);
	CHECK(kg_lisp_eval_string(truncated, 6, result, sizeof(result)) != 0);
	CHECK(strstr(result, "unclosed list") != nullptr);
	CHECK(kg_lisp_eval_string("9", 1, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "9") == 0);
	kg_lisp_shutdown();
}

static void test_eval_and_recovery(void)
{
	char result[128] = "";

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string("(+ 1 2)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "3") == 0);
	CHECK(kg_lisp_eval_string("(car 1)", 7, result, sizeof(result)) != 0);
	CHECK(strstr(result, "eval:") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 3 4)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "7") == 0);
	kg_lisp_shutdown();
}

static void test_load_file(void)
{
	char path[] = "/tmp/kg-lisp-XXXXXX";
	char result[128] = "";
	static const char source[] = "(setq loaded-value 41)\n"
				     "(+ loaded-value 1)\n";
	int fd;
	FILE *file;

	fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd < 0) {
		return;
	}
	file = fdopen(fd, "w");
	CHECK(file != nullptr);
	if (file == nullptr) {
		(void)close(fd);
		(void)unlink(path);
		return;
	}
	CHECK(
	    fwrite(source, 1, sizeof(source) - 1, file) == sizeof(source) - 1);
	CHECK(fclose(file) == 0);

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_load_file(path) == 0);
	CHECK(kg_lisp_last_error()[0] == '\0');
	CHECK(kg_lisp_eval_string("loaded-value", 12, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "41") == 0);
	kg_lisp_shutdown();
	CHECK(unlink(path) == 0);
}

static void test_load_file_error(void)
{
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_load_file("/tmp/kg-lisp-missing/no-file.el") != 0);
	CHECK(strstr(kg_lisp_last_error(), "cannot open") != nullptr);
	CHECK(strstr(kg_lisp_last_error(), "no-file.el") != nullptr);
	kg_lisp_shutdown();
}

/* Write SOURCE to a fresh temporary .el file, returning its path in
 * PATH (which must be a writable "...XXXXXX" template) or false. */
static bool write_temp_lisp(char *path, const char *source)
{
	int fd = mkstemp(path);
	size_t length = strlen(source);
	FILE *file;
	bool written;

	if (fd < 0) {
		return false;
	}
	file = fdopen(fd, "w");
	if (file == nullptr) {
		(void)close(fd);
		(void)unlink(path);
		return false;
	}
	/* Close unconditionally: short-circuiting past fclose on a failed
	 * write leaks the stream, which gcc's -fanalyzer says out loud. */
	written = fwrite(source, 1, length, file) == length;
	if (fclose(file) != 0 || !written) {
		(void)unlink(path);
		return false;
	}
	return true;
}

/* Sub-plan 11D Part 3: what a completion raised inside a loaded file can
 * and cannot reach.  The compat corpus pins the same two answers against
 * the pinned Emacs (load-error-condition-reachability, closed here, and
 * load-throw-reachability, opened here); this runs them in-process and
 * adds the parts the corpus has no way to ask about -- that `load'
 * answers t, that the loader's own bookkeeping is unwound rather than
 * merely abandoned, and that a second load after a caught error still
 * works, which is what a leaked depth counter or a leaked load_buffers
 * slot would break. */
static void test_load_error_condition_reachability(void)
{
	char raiser[] = "/tmp/kg-lisp-raise-XXXXXX";
	char thrower[] = "/tmp/kg-lisp-throw-XXXXXX";
	char plain[] = "/tmp/kg-lisp-plain-XXXXXX";
	char form[512];
	char result[256] = "";

	CHECK(write_temp_lisp(raiser, "(car 1)\n"));
	CHECK(write_temp_lisp(thrower, "(throw 'load-tag 99)\n"));
	CHECK(write_temp_lisp(plain, "(setq loaded-marker 'ran)\n"));

	CHECK(kg_lisp_init() == 0);

	/* `load' answers t, as Emacs' does. */
	(void)snprintf(form, sizeof(form), "(load \"%s\")", plain);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "t") == 0);
	CHECK(kg_lisp_eval_string("loaded-marker", 13, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "ran") == 0);

	/* The flip: an ordinary error inside the loaded file reaches an
	 * enclosing condition-case with its condition symbol intact,
	 * instead of transferring past it to kg's outermost barrier. */
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"%s\") (error (car e)))", raiser);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "wrong-type-argument") == 0);
	/* Named narrowly, not just as `error': the condition object is
	 * replayed, not re-manufactured. */
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"%s\") (wrong-type-argument 'narrow))",
	    raiser);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "narrow") == 0);

	/* The bookkeeping was unwound, not abandoned: loading again after
	 * a caught error still works.  A leaked load_depth or a leaked
	 * load_buffers slot shows up here and nowhere else. */
	(void)snprintf(form, sizeof(form), "(load \"%s\")", plain);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "t") == 0);

	/* The half that stays divergent: a throw does not cross the
	 * containment barrier, so the catch does not receive 99 and the
	 * throw arrives as no-catch. */
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (catch 'load-tag (load \"%s\"))"
	    " (error (car e)))",
	    thrower);
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "no-catch") == 0);

	/* A missing file raises plain `error', not Emacs' file-missing --
	 * recorded in the native-load row, not fixed. */
	(void)snprintf(form, sizeof(form),
	    "(condition-case e (load \"/tmp/kg-lisp-missing/x.el\")"
	    " (error (car e)))");
	CHECK(kg_lisp_eval_string(form, strlen(form), result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "error") == 0);

	kg_lisp_shutdown();
	CHECK(unlink(raiser) == 0);
	CHECK(unlink(thrower) == 0);
	CHECK(unlink(plain) == 0);
}

static int interrupt_polls;

static int cancel_evaluation(void) { return ++interrupt_polls >= 2; }

static void test_interrupt(void)
{
	char result[128] = "";

	CHECK(kg_lisp_init() == 0);
	interrupt_polls = 0;
	kg_lisp_set_interrupt_check(cancel_evaluation);
	CHECK(kg_lisp_eval_string("(while t 1)", 11, result, sizeof(result))
	    != 0);
	CHECK(strcmp(result, "Quit") == 0);
	kg_lisp_set_interrupt_check(nullptr);
	CHECK(kg_lisp_eval_string("(+ 2 3)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "5") == 0);
	kg_lisp_shutdown();
}

static void test_message_arity(void)
{
	char result[128] = "";

	setup_editor();
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string("(message)", 9, result, sizeof(result)) != 0);
	CHECK(strstr(result, "too few arguments") != nullptr);
	CHECK(kg_lisp_eval_string("(message 1)", 11, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "expected string") != nullptr);
	CHECK(kg_lisp_eval_string(
		  "(message \"ready\")", 17, result, sizeof(result))
	    == 0);
	CHECK(strcmp(test_status_message, "ready") == 0);
	/* message formats its argument, so a second one is no longer an
	 * arity error; test_format_natives covers what it does with it. */
	kg_lisp_shutdown();
	teardown_editor();
}

static void test_insert_and_undo(void)
{
	static constexpr size_t payload_len = 2048;
	static const char prefix[] = "(insert \"";
	static const char suffix[] = "\")";
	char result[128] = "";
	char *source;
	size_t source_len;

	setup_editor();
	source_len = sizeof(prefix) - 1 + payload_len + sizeof(suffix) - 1;
	source = malloc(source_len + 1);
	CHECK(source != nullptr);
	if (!source) {
		teardown_editor();
		return;
	}
	memcpy(source, prefix, sizeof(prefix) - 1);
	memset(source + sizeof(prefix) - 1, 'x', payload_len);
	memcpy(
	    source + sizeof(prefix) - 1 + payload_len, suffix, sizeof(suffix));

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string(source, source_len, result, sizeof(result))
	    == 0);
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == (int)payload_len);
	CHECK(memcmp(bcur()->row[0].chars, source + sizeof(prefix) - 1,
		  payload_len)
	    == 0);
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	CHECK(bcur()->row[0].size == 0);
	kg_lisp_shutdown();
	free(source);
	teardown_editor();
}

static void test_insert_read_only_recovery(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(bcur(), 0, "original", 8);
	bcur()->readonly = 1;
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string(
		  "(insert \"changed\")", 18, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "buffer is read-only") != nullptr);
	CHECK(bcur()->row[0].size == 8);
	CHECK(memcmp(bcur()->row[0].chars, "original", 8) == 0);
	CHECK(kg_lisp_eval_string("(+ 5 6)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "11") == 0);
	kg_lisp_shutdown();
	teardown_editor();
}

static void test_goto_line_and_buffer_name(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(bcur(), 0, "zero", 4);
	editor_insert_row(bcur(), 1, "second", 6);
	editor_insert_row(bcur(), 2, "third", 5);
	CHECK(kg_lisp_init() == 0);
	/* Lines count from 1 and point lands at the beginning of the line, so
	 * line 2 of "zero\nsecond\nthird" is position 6. */
	CHECK(kg_lisp_eval_string("(goto-line 2)", 13, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("(point)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "6") == 0);
	/* A column is reached by moving on from there. */
	CHECK(kg_lisp_eval_string(
		  "(goto-char (+ (point) 2))", 25, result, sizeof(result))
	    == 0);
	CHECK(
	    kg_lisp_eval_string("(current-column)", 16, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "2") == 0);
	/* Out-of-range lines clamp to the buffer. */
	CHECK(
	    kg_lisp_eval_string("(goto-line 9999)", 16, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string(
		  "(line-number-at-pos)", 20, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "3") == 0);
	CHECK(kg_lisp_eval_string("(goto-line -9)", 14, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("(point)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "1") == 0);
	CHECK(kg_lisp_eval_string("(buffer-name)", 13, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "bridge.txt") == 0);
	kg_lisp_shutdown();
	teardown_editor();
}

static int write_text_file(const char *path, const char *content)
{
	FILE *file = fopen(path, "w");

	if (!file) {
		return 1;
	}
	if (fputs(content, file) == EOF) {
		(void)fclose(file);
		return 1;
	}
	return fclose(file) != 0;
}

/* Create <root>/kg and <root>/kg/lisp under a fresh temp dir and point
 * XDG_CONFIG_HOME at it. */
static int setup_config_root(char *root, size_t rootsize)
{
	char path[512];

	(void)snprintf(root, rootsize, "/tmp/kg-lisp-cfg-XXXXXX");
	if (!mkdtemp(root)) {
		return 1;
	}
	(void)snprintf(path, sizeof(path), "%s/kg", root);
	if (mkdir(path, 0700) != 0) {
		return 1;
	}
	(void)snprintf(path, sizeof(path), "%s/kg/lisp", root);
	if (mkdir(path, 0700) != 0) {
		return 1;
	}
	return setenv("XDG_CONFIG_HOME", root, 1) != 0;
}

static void remove_config_root(const char *root)
{
	static const char *const entries[] = {
		"%s/kg/init.fe",
		"%s/kg/init.el",
		"%s/kg/lisp/legacy.fe",
		"%s/kg/lisp/pkg.el",
		"%s/kg/lisp/pkg-a.el",
		"%s/kg/lisp/pkg-b.el",
		"%s/kg/lisp/pkg-x.el",
		"%s/kg/lisp/oldstyle.fe",
		"%s/kg/lisp/counted.el",
		"%s/kg/lisp/quiet.el",
		"%s/kg/lisp/after-cycle.el",
		"%s/kg/lisp/impl.el",
		"%s/kg/lisp/dup.el",
		"%s/kg/lisp/suffixed.el",
		"%s/kg/lisp/plainstem.el",
		"%s/kg/lisp/doubled.el.el",
		"%s/absolute-pkg.el",
		"%s/direct.fe",
		"%s/literal.el",
		"%s/kg/lisp",
		"%s/kg",
		"%s",
	};
	char path[512];
	size_t i;

	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		(void)snprintf(path, sizeof(path), entries[i], root);
		(void)remove(path);
	}
	(void)unsetenv("XDG_CONFIG_HOME");
}

static void test_init_file(void)
{
	char root[64], path[512], result[128] = "";

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* Missing init file is normal. */
	CHECK(kg_lisp_load_init() == 0);

	/* An init.fe with no init.el alongside it is ignored: discovery is
	 * .el-only, with no probing of the retired extension. */
	(void)snprintf(path, sizeof(path), "%s/kg/init.fe", root);
	CHECK(write_text_file(path, "(setq init-loaded 99)\n") == 0);
	CHECK(kg_lisp_load_init() == 0);
	CHECK(kg_lisp_eval_string("init-loaded", 11, result, sizeof(result))
	    != 0);
	CHECK(strstr(kg_lisp_last_error(), "void-variable") != nullptr);

	/* Adding init.el is then loaded. */
	(void)snprintf(path, sizeof(path), "%s/kg/init.el", root);
	CHECK(write_text_file(path, "(setq init-loaded 42)\n") == 0);
	CHECK(kg_lisp_load_init() == 0);
	CHECK(kg_lisp_eval_string("init-loaded", 11, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "42") == 0);

	/* A broken init file reports its labelled error; forms evaluated
	 * before the failure remain applied. */
	CHECK(write_text_file(path, "(setq init-partial 1)\n(car 1)\n") == 0);
	CHECK(kg_lisp_load_init() != 0);
	CHECK(strstr(kg_lisp_last_error(), "init.el") != nullptr);
	CHECK(kg_lisp_eval_string("init-partial", 12, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "1") == 0);

	kg_lisp_shutdown();
	remove_config_root(root);
}

static void test_load_package(void)
{
	static const char load_error_form[]
	    = "(condition-case e (load \"caught-load-error\")"
	      " (error 'caught))";
	char root[64], path[512], source[600], result[128] = "";
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* A real NAME.fe on disk does not satisfy bare (load "NAME"): no
	 * fallback probes the retired extension, and the error names the
	 * .el path discovery actually looked for. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/legacy.fe", root);
	CHECK(write_text_file(path, "(setq legacy-value 9)\n") == 0);
	CHECK(
	    kg_lisp_eval_string("(load \"legacy\")", 15, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "legacy.el") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 1 1)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "2") == 0);

	/* Bare names resolve to <config>/kg/lisp/NAME.el. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg.el", root);
	CHECK(write_text_file(path, "(setq pkg-value 7)\n") == 0);
	CHECK(kg_lisp_eval_string("(load \"pkg\")", 12, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("pkg-value", 9, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "7") == 0);

	/* The .el suffix may be written out, as in Emacs, whose `load`
	 * tries NAME.elc, NAME.el and then NAME so both spellings find the
	 * same file.  kg used to build pkg.el.el and report a path nobody
	 * asked for. */
	CHECK(
	    kg_lisp_eval_string("(load \"pkg.el\")", 15, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("pkg-value", 9, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "7") == 0);

	/* Missing packages raise an error naming the resolved path. */
	CHECK(
	    kg_lisp_eval_string("(load \"absent\")", 15, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "absent.el") != nullptr);
	/* ... and with the suffix written out, the same path, not
	 * absent.el.el. */
	CHECK(kg_lisp_eval_string(
		  "(load \"absent.el\")", 18, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "absent.el") != nullptr);
	CHECK(strstr(result, "absent.el.el") == nullptr);
	CHECK(kg_lisp_eval_string("(+ 1 1)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "2") == 0);

	/* Packages may load packages. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-a.el", root);
	CHECK(
	    write_text_file(path, "(load \"pkg-b\")\n(setq a-after b-value)\n")
	    == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-b.el", root);
	CHECK(write_text_file(path, "(setq b-value 5)\n") == 0);
	CHECK(
	    kg_lisp_eval_string("(load \"pkg-a\")", 14, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("a-after", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "5") == 0);

	/* Load cycles hit the depth limit and recover. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-x.el", root);
	CHECK(write_text_file(path, "(load \"pkg-x\")\n") == 0);
	CHECK(
	    kg_lisp_eval_string("(load \"pkg-x\")", 14, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "depth limit") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 2 2)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "4") == 0);

	/* Names containing '/' are literal paths, independent of extension:
	 * this is a bare .el literal, not the .fe regression -- see
	 * test_require_provide's direct.fe case for that. */
	(void)snprintf(path, sizeof(path), "%s/literal.el", root);
	CHECK(write_text_file(path, "(setq literal-value 3)\n") == 0);
	length = snprintf(source, sizeof(source), "(load \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(
	    kg_lisp_eval_string(source, (size_t)length, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("literal-value", 13, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "3") == 0);

	/* An evaluation error raised by a loaded file reaches a
	 * condition-case around the load, since sub-plan 11D Part 3 put
	 * lisp_eval_file() on fe's protected string entry.  It used to
	 * cross the nested evaluator's barrier and surface at top level,
	 * and this assertion is the inverted form of the one that pinned
	 * that: the same form now RETURNS, with the handler's value.
	 * The path resolution here is the bare-name one -- <config>/kg/lisp
	 * -- which the reachability fix had to leave alone. */
	(void)snprintf(
	    path, sizeof(path), "%s/kg/lisp/caught-load-error.el", root);
	CHECK(write_text_file(path, "(car 1)\n") == 0);
	CHECK(kg_lisp_eval_string(load_error_form, sizeof(load_error_form) - 1,
		  result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "caught") == 0);
	CHECK(kg_lisp_eval_string("(+ 2 3)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "5") == 0);

	kg_lisp_shutdown();
	remove_config_root(root);
}

static void test_command_allow_list(void)
{
	char result[128] = "";

	setup_editor();
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string("(command-execute \"shell-command\")", 33,
		  result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "not allowed") != nullptr);
	CHECK(strstr(result, "shell-command") != nullptr);
	CHECK(test_command_calls == 0);
	CHECK(kg_lisp_eval_string(
		  "(command-execute \"version\")", 27, result, sizeof(result))
	    == 0);
	CHECK(test_command_calls == 1);
	CHECK(strcmp(test_command_name, "version") == 0);
	/* A quoted symbol names the command as it does in Emacs. */
	CHECK(kg_lisp_eval_string(
		  "(command-execute 'version)", 26, result, sizeof(result))
	    == 0);
	CHECK(test_command_calls == 2);
	CHECK(strcmp(test_command_name, "version") == 0);
	/* A read-only buffer refuses a command that edits it, and says so
	 * as that rather than as "not allowed" -- the two verdicts are
	 * different answers and a caller may want to tell them apart. */
	bcur()->readonly = 1;
	CHECK(kg_lisp_eval_string("(command-execute \"upcase-word\")", 31,
		  result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "read-only") != nullptr);
	CHECK(test_command_calls == 2);
	bcur()->readonly = 0;
	kg_lisp_shutdown();
	teardown_editor();
}

/* Defined below, beside the tests that use it most. */
static int eval_eq(const char *source, const char *expected);

static int eval_ok(const char *source)
{
	char result[256] = "";
	return kg_lisp_eval_string(
		   source, strlen(source), result, sizeof(result))
	    == 0;
}

static int eval_error_contains(const char *source, const char *fragment)
{
	char result[256] = "";

	if (kg_lisp_eval_string(source, strlen(source), result, sizeof(result))
	    == 0) {
		return 0;
	}
	return strstr(result, fragment) != nullptr;
}

static void test_define_and_run_command(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(kg_lisp_run_command("greet", 0) != 0);
	CHECK(eval_ok(
	    "(define-command \"greet\" (lambda () (message \"hello\")))"));
	CHECK(kg_lisp_command_name(0) != nullptr);
	CHECK(strcmp(kg_lisp_command_name(0), "greet") == 0);
	CHECK(kg_lisp_command_name(1) == nullptr);
	CHECK(kg_lisp_run_command("greet", 0) == 0);
	CHECK(strcmp(test_status_message, "hello") == 0);

	/* Redefinition replaces the function (and releases the old root). */
	CHECK(eval_ok(
	    "(define-command \"greet\" (lambda () (message \"again\")))"));
	CHECK(kg_lisp_run_command("greet", 0) == 0);
	CHECK(strcmp(test_status_message, "again") == 0);
	CHECK(kg_lisp_command_name(1) == nullptr);

	/* Errors inside a command return to the caller with a message that
	 * names the command, as the trampoline's eval label did. */
	CHECK(eval_ok("(define-command \"boom\" (lambda () (car 1)))"));
	CHECK(kg_lisp_run_command("boom", 0) == 0);
	CHECK(strstr(test_status_message, "Lisp error") != nullptr);
	CHECK(strstr(test_status_message, "boom") != nullptr);
	CHECK(kg_lisp_run_command("greet", 0) == 0);
	CHECK(strcmp(test_status_message, "again") == 0);

	/* A runaway command hits the step budget and recovers. */
	CHECK(eval_ok("(define-command \"spin\" (lambda () (while t 1)))"));
	CHECK(kg_lisp_run_command("spin", 0) == 0);
	CHECK(strstr(test_status_message, "step limit") != nullptr);
	CHECK(eval_ok("(+ 1 2)"));

	CHECK(eval_ok("(remove-command \"boom\")"));
	CHECK(kg_lisp_run_command("boom", 0) != 0);
	CHECK(eval_error_contains(
	    "(remove-command \"boom\")", "no such Lisp command"));

	CHECK(eval_error_contains(
	    "(define-command \"version\" (lambda () 1))", "built-in"));
	CHECK(eval_error_contains(
	    "(define-command \"x\" 1)", "requires a function"));
	/* The optional third and fourth arguments are validated too: a spec
	 * is nil, a string, or a zero-argument function, and documentation
	 * is nil or a string. */
	CHECK(eval_error_contains("(define-command \"x\" (lambda () 1) 5)",
	    "requires a string or function spec"));
	CHECK(eval_error_contains("(define-command \"x\" (lambda () 1) nil 5)",
	    "documentation requires a string"));
	/* ... and both optional arguments really are optional. */
	CHECK(eval_ok("(define-command \"two-arg\" (lambda () 1))"));
	CHECK(eval_ok("(remove-command \"two-arg\")"));

	/* lookup-key answers nil rather than raising for a map nothing is
	 * called, and for a sequence that does not parse. */
	CHECK(eval_eq("(lookup-key \"no-such-map\" \"C-c i\")", "nil"));
	CHECK(
	    eval_eq("(lookup-key \"global\" \"C-c C-c C-c C-c C-c\")", "nil"));

	/* global-unset-key tells an unbindable sequence from an unbound
	 * one, and names the sequence in both messages. */
	CHECK(eval_error_contains(
	    "(global-unset-key \"C-x i\")", "only \"C-c <key>\" is bindable"));
	CHECK(eval_error_contains(
	    "(global-unset-key \"C-c q\")", "key is not bound"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* 07D item 3: a failed registration leaves both the old command and the
 * old function cell intact, and the command root and the function cell
 * hold one closure rather than two evaluations that merely behave alike.
 *
 * The probe is the command table filling up.  `defun` used to expand to
 * defalias-then-define-command, reading the cell back with
 * (symbol-function 'name) for the second step, so a define-command that
 * could not find a slot had already replaced the function. */
/* 07E §2's numeric classifier, driven directly: it is pure, so this is
 * the seam a native test can reach.  The `n`/`N` reader used to hand the
 * typed text straight to strtoimax and then to strtod and read the answer
 * off `end` and `errno`, which accepted far too much -- every NONE case
 * below was a silently accepted number before, and the empty one was 0
 * (with the keystrokes after it falling into the buffer). */
static void test_number_token_classifier(void)
{
	/* Integers, including fe's `5.` spelling. */
	CHECK(kg_number_token_classify("0") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("42") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("-42") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("+42") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("5.") == KG_NUMBER_TOKEN_INTEGER);
	/* ... and only trailing/leading ASCII whitespace beside them. */
	CHECK(kg_number_token_classify("  7  ") == KG_NUMBER_TOKEN_INTEGER);
	CHECK(kg_number_token_classify("7\t") == KG_NUMBER_TOKEN_INTEGER);

	/* Floats: fraction and exponent forms. */
	CHECK(kg_number_token_classify("5.0") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify(".5") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("-.5") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1e3") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1E3") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1e-3") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1.5e+10") == KG_NUMBER_TOKEN_FLOAT);
	CHECK(kg_number_token_classify("1.e3") == KG_NUMBER_TOKEN_FLOAT);

	/* Nothing at all, which used to be the integer 0. */
	CHECK(kg_number_token_classify("") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("   ") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("\t\n") == KG_NUMBER_TOKEN_NONE);

	/* Not numbers, all of which the two converters between them
	 * accepted: strtod reads inf/nan and hex, and strtoimax stopped at
	 * the `x` and reported a clean 0. */
	CHECK(kg_number_token_classify("inf") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("-inf") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("nan") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("NaN") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("infinity") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("0x10") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1e+INF") == KG_NUMBER_TOKEN_NONE);

	/* Trailing and interior junk, and a second token. */
	CHECK(kg_number_token_classify("12abc") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1.2.3") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1 2") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1e") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("1e+") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("+") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify("-") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify(".") == KG_NUMBER_TOKEN_NONE);
	CHECK(kg_number_token_classify(".e3") == KG_NUMBER_TOKEN_NONE);
	/* No Lisp is evaluated to decide: a form is not a number. */
	CHECK(kg_number_token_classify("(+ 1 2)") == KG_NUMBER_TOKEN_NONE);

	/* Overflow is still an integer *token*; the reader turns it into a
	 * double, as an integer literal past int64 does in fe's reader. */
	CHECK(kg_number_token_classify("99999999999999999999")
	    == KG_NUMBER_TOKEN_INTEGER);
}

/* 07E: prompting is refused outside a real key/M-x command context.
 *
 * These tests have no terminal, so cmd_prompt_fd() answers -1 -- which
 * is exactly the shape of loading an init file, running a hook, or
 * running a process filter: a live Lisp frame with no command fd behind
 * it.  Guessing from state.frame_active, which is true in all of those,
 * is what the slice was told not to do, and this is the observable
 * difference.  The command body must not run either. */
static void test_interactive_prompt_context_refusal(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq ran nil)"));
	CHECK(eval_ok("(defun asks (v) (interactive \"sV: \") (setq ran v))"));
	CHECK(kg_lisp_run_command("asks", 0) == 0);
	CHECK(strstr(test_status_message,
		  "interactive prompt is not available here")
	    != nullptr);
	CHECK(eval_eq("ran", "nil"));

	/* Reached from Lisp, the same refusal is an ordinary condition the
	 * calling program can catch, and still does not run the body. */
	CHECK(eval_eq("(condition-case e (command-execute 'asks)"
		      "  (error 'refused))",
	    "refused"));
	CHECK(eval_eq("ran", "nil"));

	/* A command that needs no prompt is unaffected. */
	CHECK(eval_ok("(defun quiet () (interactive) (setq ran 'yes))"));
	CHECK(kg_lisp_run_command("quiet", 0) == 0);
	CHECK(eval_eq("ran", "yes"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_defun_redefinition_is_atomic(void)
{
	char name[32];
	char form[128];
	int i;

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* An ordinary, non-interactive function first. */
	CHECK(eval_ok("(defun old-fn () 111)"));
	CHECK(eval_eq("(old-fn)", "111"));
	CHECK(kg_lisp_command_exists("old-fn") == 0);

	/* Fill the 32-slot command table with other names. */
	for (i = 0; i < 32; i++) {
		(void)snprintf(name, sizeof(name), "filler-%d", i);
		(void)snprintf(form, sizeof(form),
		    "(define-command \"%s\" (lambda () 0))", name);
		CHECK(eval_ok(form));
	}
	CHECK(eval_error_contains(
	    "(define-command \"one-more\" (lambda () 0))", "too many"));

	/* Redefining old-fn interactively must now fail -- and fail whole. */
	CHECK(eval_error_contains(
	    "(defun old-fn () (interactive) 222)", "too many"));
	CHECK(eval_eq("(old-fn)", "111"));
	CHECK(kg_lisp_command_exists("old-fn") == 0);

	/* With a slot free again it succeeds, and the command and the
	 * function cell are the same object -- `eq`, not merely equal. */
	CHECK(eval_ok("(remove-command \"filler-0\")"));
	CHECK(eval_ok("(defun old-fn () (interactive) 222)"));
	CHECK(eval_eq("(old-fn)", "222"));
	CHECK(kg_lisp_command_exists("old-fn") == 1);
	CHECK(kg_lisp_run_command("old-fn", 0) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_run_command_interrupt(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(define-command \"spin\" (lambda () (while t 1)))"));
	/* C-g reaches the command's loop through the same interrupt check
	 * eval-expression uses. */
	interrupt_polls = 0;
	kg_lisp_set_interrupt_check(cancel_evaluation);
	CHECK(kg_lisp_run_command("spin", 0) == 0);
	CHECK(strcmp(test_status_message, "Quit") == 0);
	kg_lisp_set_interrupt_check(nullptr);

	/* A cancelled run leaves the interpreter usable, and without the
	 * interrupt the same command now exhausts the step budget.  That
	 * report names the command, which is what tells a user *which* of
	 * their commands ran away; the quit above deliberately does not,
	 * because "Quit" is the whole message Emacs shows for a C-g. */
	CHECK(kg_lisp_run_command("spin", 0) == 0);
	CHECK(strstr(test_status_message, "step limit") != nullptr);
	CHECK(strstr(test_status_message, "Lisp error: spin:") != nullptr);
	CHECK(eval_ok("(+ 1 1)"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_run_command_reentrancy(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* A Lisp-defined command IS CMD_LISP_CALLABLE, so (command-execute
	 * 'inner) is a real nested activation.  The outer command has a live
	 * top-level frame; the nested one must build and call inside that
	 * evaluator rather than installing a frame of its own.  Overwriting
	 * the single state.frame here -- and clearing frame_active on the way
	 * out, while the outer command was still running -- is what made the
	 * outer command's next buffer operation abort(). */
	CHECK(eval_ok("(defun inner () (interactive) 1)"));
	CHECK(eval_ok("(defun outer ()"
		      "  (interactive)"
		      "  (setq outer-saw (command-execute 'inner))"
		      "  (insert \"after\")"
		      "  (message \"outer-done\"))"));
	CHECK(kg_lisp_run_command("outer", 0) == 0);
	/* command-execute returns the command's value, as Emacs does and as
	 * the checked-in oracle snapshot says. */
	CHECK(eval_eq("outer-saw", "1"));
	/* The outer command kept running after the nested one returned: a
	 * buffer operation past the nested call is exactly what used to
	 * raise "current buffer is dead" and abort. */
	CHECK(strcmp(test_status_message, "outer-done") == 0);
	CHECK(strstr(bcur()->row[0].chars, "after") != nullptr);

	/* A nested failure propagates into the surrounding evaluator, so the
	 * outer command's own condition-case catches it and the outer
	 * command completes normally. */
	CHECK(eval_ok("(defun boom () (interactive) (car 1 2))"));
	CHECK(eval_ok("(defun guarded ()"
		      "  (interactive)"
		      "  (setq guarded-saw"
		      "    (condition-case e (command-execute 'boom)"
		      "      (wrong-number-of-arguments 'caught)))"
		      "  (message \"guarded-done\"))"));
	CHECK(kg_lisp_run_command("guarded", 0) == 0);
	CHECK(eval_eq("guarded-saw", "caught"));
	CHECK(strcmp(test_status_message, "guarded-done") == 0);

	/* Uncaught, the nested failure is reported against the *outer*
	 * command, whose frame is the one that recovers. */
	CHECK(eval_ok("(defun bare () (interactive) (command-execute 'boom))"));
	CHECK(kg_lisp_run_command("bare", 0) == 0);
	CHECK(strstr(test_status_message, "Lisp error: bare:") != nullptr);
	/* and the interpreter is usable afterwards. */
	CHECK(eval_ok("(+ 1 1)"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* 07D's prefix delivery, end to end through cmd_invoke: the raw form a
 * keystroke committed must reach `P`, `p` and current-prefix-arg.
 *
 * The regression these pin is that handle_pending_universal_arg() cleared
 * editor.prefix_raw_kind on the *commit* path, before the dispatcher
 * copied it, so every Lisp command saw raw nil however the argument was
 * spelled: P nil, p 1, current-prefix-arg nil. */
static void test_command_prefix_delivery(void)
{
	struct command_prefix none = { 0, 0, PREFIX_RAW_NONE, 0 };
	struct command_prefix five = { 1, 5, PREFIX_RAW_INTEGER, 0 };
	struct command_prefix minus = { 1, -1, PREFIX_RAW_MINUS, 0 };
	struct command_prefix one_cu = { 1, 4, PREFIX_RAW_UNIVERSAL, 1 };
	struct command_prefix two_cu = { 1, 16, PREFIX_RAW_UNIVERSAL, 2 };
	struct command_prefix five_cu = { 1, 1000, PREFIX_RAW_UNIVERSAL, 5 };

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(defun raw () (interactive \"P\") (setq seen p))"));
	CHECK(eval_ok("(defun num (n) (interactive \"p\") (setq seen n))"));
	/* `P` is the argument, and it is `eq` to current-prefix-arg during
	 * the body -- one rooted object, never two. */
	CHECK(eval_ok("(defun both (x)"
		      "  (interactive \"P\")"
		      "  (setq seen (eq x current-prefix-arg)))"));

	CHECK(eval_ok("(defun raw1 (x) (interactive \"P\") (setq seen x))"));
	CHECK(test_run_command_with_prefix("raw1", none) == CMD_RAN);
	CHECK(eval_eq("seen", "nil"));
	CHECK(test_run_command_with_prefix("raw1", five) == CMD_RAN);
	CHECK(eval_eq("seen", "5"));
	CHECK(test_run_command_with_prefix("raw1", minus) == CMD_RAN);
	CHECK(eval_eq("seen", "-"));
	CHECK(test_run_command_with_prefix("raw1", one_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "(4)"));
	CHECK(test_run_command_with_prefix("raw1", two_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "(16)"));
	/* The 1000 cap is the *effective* integer's, not the raw list's:
	 * five bare C-u is (1024) in Emacs and was (1000) here. */
	CHECK(test_run_command_with_prefix("raw1", five_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "(1024)"));

	/* Emacs would use a bignum past int64; the raw list saturates at the
	 * largest representable power of 4 instead, which needs 32
	 * consecutive C-u and is a recorded limit rather than a wrap. */
	{
		struct command_prefix many
		    = { 1, 1000, PREFIX_RAW_UNIVERSAL, 40 };

		CHECK(test_run_command_with_prefix("raw1", many) == CMD_RAN);
		CHECK(eval_eq("(car seen)", "4611686018427387904"));
	}

	/* `p` is prefix-numeric-value of the same raw form. */
	CHECK(test_run_command_with_prefix("num", none) == CMD_RAN);
	CHECK(eval_eq("seen", "1"));
	CHECK(test_run_command_with_prefix("num", five) == CMD_RAN);
	CHECK(eval_eq("seen", "5"));
	CHECK(test_run_command_with_prefix("num", minus) == CMD_RAN);
	CHECK(eval_eq("seen", "-1"));
	CHECK(test_run_command_with_prefix("num", two_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "16"));

	CHECK(test_run_command_with_prefix("both", two_cu) == CMD_RAN);
	CHECK(eval_eq("seen", "t"));

	/* Each command builds its own raw list, so mutating one cannot be
	 * seen by the next invocation. */
	CHECK(eval_ok("(defun grab (x) (interactive \"P\") (setq kept x))"));
	CHECK(test_run_command_with_prefix("grab", one_cu) == CMD_RAN);
	CHECK(eval_ok("(setcar kept 99)"));
	CHECK(test_run_command_with_prefix("grab", one_cu) == CMD_RAN);
	CHECK(eval_eq("kept", "(4)"));

	/* current-prefix-arg is restored after a normal exit, and the prior
	 * global is read rather than assumed nil. */
	CHECK(eval_ok("(setq current-prefix-arg 'outside)"));
	CHECK(test_run_command_with_prefix("raw1", five) == CMD_RAN);
	CHECK(eval_eq("current-prefix-arg", "outside"));
	/* ... and after an error exit. */
	CHECK(eval_ok("(defun bang () (interactive) (car 1 2))"));
	CHECK(test_run_command_with_prefix("bang", five) == CMD_RAN);
	CHECK(eval_eq("current-prefix-arg", "outside"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_key_bindings(void)
{
	char spelled[32];

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(keybind_parse("C-c i", spelled, sizeof(spelled)) == 0
	    && strcmp(spelled, "C-c i") == 0);
	CHECK(keybind_parse("C-c C-y", spelled, sizeof(spelled)) == 0
	    && strcmp(spelled, "C-c C-y") == 0);
	CHECK(keybind_parse("C-x i", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c", spelled, sizeof(spelled)) != 0);
	/* Extra spaces are separators now, so this one is bindable; what
	 * matters is that it means exactly what "C-c i" means. */
	CHECK(keybind_parse("C-c  i", spelled, sizeof(spelled)) == 0
	    && strcmp(spelled, "C-c i") == 0);
	CHECK(keybind_parse("C-c ii", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c C-g", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c C-c", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c C-x", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c M-f", spelled, sizeof(spelled)) != 0);
	CHECK(keybind_parse("C-c SPC", spelled, sizeof(spelled)) != 0);

	CHECK(eval_ok(
	    "(define-command \"greet\" (lambda () (message \"hey\")))"));
	CHECK(eval_ok("(global-set-key \"C-c i\" \"greet\")"));
	CHECK(keybind_lookup("C-c i") != nullptr);
	CHECK(strcmp(keybind_lookup("C-c i"), "greet") == 0);
	CHECK(eval_error_contains(
	    "(global-set-key \"C-x i\" \"greet\")", "invalid key sequence"));
	CHECK(eval_ok("(global-unset-key \"C-c i\")"));
	CHECK(keybind_lookup("C-c i") == nullptr);
	CHECK(eval_error_contains("(global-unset-key \"C-c i\")", "not bound"));

	kg_lisp_shutdown();
	teardown_editor();
}

static int eval_eq(const char *source, const char *expected)
{
	char result[256] = "";

	if (kg_lisp_eval_string(source, strlen(source), result, sizeof(result))
	    != 0) {
		return 0;
	}
	return strcmp(result, expected) == 0;
}

/* provide/require/featurep: the bounded feature table, cycle detection,
 * and require's FILENAME argument.  Load-path order and a load-path full
 * of stale directories are separate tests below, since they need
 * add-to-load-path and a second temp directory outside the config
 * root. */
static void test_require_provide(void)
{
	char root[64], path[512], source[600];
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(featurep 'counted)", "nil"));

	/* A second require is a no-op: proven by a counter the file only
	 * bumps when it actually runs, not just by the absence of an
	 * error. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/counted.el", root);
	CHECK(write_text_file(path,
		  "(setq counted-runs "
		  "(if (boundp 'counted-runs) (+ counted-runs 1) 1))\n"
		  "(provide 'counted)\n")
	    == 0);
	CHECK(eval_eq("(require 'counted)", "counted"));
	CHECK(eval_eq("(featurep 'counted)", "t"));
	CHECK(eval_eq("counted-runs", "1"));
	CHECK(eval_eq("(require 'counted)", "counted"));
	CHECK(eval_eq("counted-runs", "1"));

	/* A file that never calls (provide ...) is an error naming the
	 * feature that did not appear. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/quiet.el", root);
	CHECK(write_text_file(path, "(setq quiet-ran t)\n") == 0);
	CHECK(eval_error_contains("(require 'quiet)", "quiet"));
	CHECK(eval_eq("quiet-ran", "t"));

	/* A real NAME.fe in a load-path directory does not satisfy bare
	 * (require 'NAME): the error names the feature, with no fallback
	 * to the retired extension. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/oldstyle.fe", root);
	CHECK(write_text_file(path, "(provide 'oldstyle)\n") == 0);
	CHECK(eval_error_contains("(require 'oldstyle)", "oldstyle"));
	CHECK(eval_eq("(featurep 'oldstyle)", "nil"));

	/* Cycle: pkg-a requires pkg-b, pkg-b requires pkg-a. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-a.el", root);
	CHECK(
	    write_text_file(path, "(require 'pkg-b)\n(provide 'pkg-a)\n") == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-b.el", root);
	CHECK(
	    write_text_file(path, "(require 'pkg-a)\n(provide 'pkg-b)\n") == 0);
	CHECK(eval_error_contains("(require 'pkg-a)", "pkg-a"));
	CHECK(eval_eq("(featurep 'pkg-a)", "nil"));
	/* Recovers cleanly: an unrelated eval afterwards still works. */
	CHECK(eval_eq("(+ 1 1)", "2"));
	/* And the abandoned requiring stack is actually reset, not merely
	 * unread: a later require of a fresh feature must load rather than
	 * trip the cycle check on a name left behind by the longjmp. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/after-cycle.el", root);
	CHECK(write_text_file(path, "(provide 'after-cycle)\n") == 0);
	CHECK(eval_eq("(require 'after-cycle)", "after-cycle"));
	CHECK(eval_eq("(featurep 'after-cycle)", "t"));

	/* A condition caught within the same evaluator run never reaches kg's
	 * outer frame recovery.  The require stack therefore needs an Fe
	 * cleanup of its own: retrying a missing feature must repeat the path
	 * error, not report a stale cycle left by the first non-local exit. */
	CHECK(eval_eq("(list "
		      " (condition-case e (require 'retry-absent)"
		      "  (error (car (cdr e))))"
		      " (condition-case e (require 'retry-absent)"
		      "  (error (car (cdr e)))))",
	    "(\"cannot find in load-path: retry-absent\""
	    " \"cannot find in load-path: retry-absent\")"));

	/* An explicit FILENAME resolves instead of the feature's own
	 * name. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/impl.el", root);
	CHECK(write_text_file(path, "(provide 'aliased)\n") == 0);
	CHECK(eval_ok("(require 'aliased \"impl\")"));
	CHECK(eval_eq("(featurep 'aliased)", "t"));

	/* Literal-path regression: an explicit '/'-containing filename is
	 * never extension-probed, so a real .fe file stays reachable through
	 * both load and require's own FILENAME argument -- the hard cut is
	 * discovery-only.  require runs first, while the feature is still
	 * absent: an earlier load would already provide it and require would
	 * return without exercising path resolution at all. */
	(void)snprintf(path, sizeof(path), "%s/direct.fe", root);
	CHECK(write_text_file(
		  path, "(setq direct-value 3)\n(provide 'direct-feature)\n")
	    == 0);
	CHECK(eval_eq("(featurep 'direct-feature)", "nil"));
	length = snprintf(
	    source, sizeof(source), "(require 'direct-feature \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(eval_eq(source, "direct-feature"));
	CHECK(eval_eq("(featurep 'direct-feature)", "t"));
	CHECK(eval_eq("direct-value", "3"));
	length = snprintf(source, sizeof(source), "(load \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(eval_ok(source));

	kg_lisp_shutdown();
	remove_config_root(root);
}

/* require's FILENAME and load's NAME answer the same question about the
 * same input (sub-plan 10C Part 2, 10A Decision 7).
 *
 * Before this, `(load "impl.el")` worked and `(require 'f "impl.el")`
 * did not: load's lisp_package_path() asked lisp_has_el_suffix() and
 * require's candidate_readable() unconditionally appended, so require
 * looked for impl.el.el and reported a path the caller never wrote.  Two
 * loaders in one editor disagreeing about one string.
 *
 * Every other resolution rule is pinned here *unchanged*, because the
 * fix is one conditional inside the load-path candidate builder and
 * nothing else may have moved: a bare stem still gains .el, a stem whose
 * file is absent is still an error naming it, and a '/'-containing name
 * is still literal -- never suffixed, never searched. */
static void test_require_el_suffix(void)
{
	char root[64], path[512], source[600];
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* The fix: a FILENAME that already ends in .el resolves to that
	 * file. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/suffixed.el", root);
	CHECK(write_text_file(
		  path, "(setq suffixed-ran t)\n(provide 'suffixed-f)\n")
	    == 0);
	CHECK(eval_eq("(require 'suffixed-f \"suffixed.el\")", "suffixed-f"));
	CHECK(eval_eq("(featurep 'suffixed-f)", "t"));
	CHECK(eval_eq("suffixed-ran", "t"));
	/* And load, which already worked, still answers the same file --
	 * the symmetry is the point of the change. */
	CHECK(eval_ok("(load \"suffixed.el\")"));

	/* Pinned unchanged: a bare stem still gains the suffix. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/plainstem.el", root);
	CHECK(write_text_file(path, "(provide 'plain-f)\n") == 0);
	CHECK(eval_eq("(require 'plain-f \"plainstem\")", "plain-f"));

	/* Pinned unchanged, and the sharp end of the fix: the doubled name
	 * is no longer what require looks for.  This file is exactly what
	 * the old code resolved "doubled.el" to, and it must now be
	 * unreachable -- the require fails naming the feature, and the
	 * marker the file would set stays unbound. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/doubled.el.el", root);
	CHECK(write_text_file(
		  path, "(setq doubled-ran t)\n(provide 'doubled-f)\n")
	    == 0);
	CHECK(eval_error_contains(
	    "(require 'doubled-f \"doubled.el\")", "cannot find in load-path"));
	CHECK(eval_eq("(featurep 'doubled-f)", "nil"));
	CHECK(eval_error_contains("doubled-ran", "doubled-ran"));

	/* Pinned unchanged: an absolute FILENAME is literal.  It contains a
	 * '/', so resolve_require_path never reaches the load-path or the
	 * suffix rule at all -- the file is taken exactly as written, .el
	 * suffix included. */
	(void)snprintf(path, sizeof(path), "%s/absolute-pkg.el", root);
	CHECK(write_text_file(
		  path, "(setq absolute-ran t)\n(provide 'absolute-f)\n")
	    == 0);
	length = snprintf(
	    source, sizeof(source), "(require 'absolute-f \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(eval_eq(source, "absolute-f"));
	CHECK(eval_eq("absolute-ran", "t"));

	kg_lisp_shutdown();
	remove_config_root(root);
}

static void test_load_path_order(void)
{
	char root[64], extra[64], path[512], source[600];
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	(void)snprintf(path, sizeof(path), "%s/kg/lisp/dup.el", root);
	CHECK(write_text_file(
		  path, "(setq dup-source 'default)\n(provide 'dup)\n")
	    == 0);

	(void)snprintf(extra, sizeof(extra), "/tmp/kg-lisp-extra-XXXXXX");
	CHECK(mkdtemp(extra) != nullptr);
	(void)snprintf(path, sizeof(path), "%s/dup.el", extra);
	CHECK(
	    write_text_file(path, "(setq dup-source 'extra)\n(provide 'dup)\n")
	    == 0);

	/* add-to-load-path prepends, so the directory just added is searched
	 * before the default <config>/kg/lisp/ one -- the order that decides
	 * which of the two same-named files (require 'dup) finds. */
	length = snprintf(
	    source, sizeof(source), "(add-to-load-path \"%s\")", extra);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(eval_ok(source));
	CHECK(eval_ok("(require 'dup)"));
	CHECK(eval_eq("dup-source", "extra"));

	kg_lisp_shutdown();
	remove_config_root(root);
	(void)snprintf(path, sizeof(path), "%s/dup.el", extra);
	(void)remove(path);
	(void)rmdir(extra);
}

static void test_load_path_missing_dirs(void)
{
	char root[64];

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* A load-path entry that does not exist fails the same access()
	 * check a missing file would, so several stale directories in a row
	 * are just several misses, not a crash. */
	CHECK(eval_ok("(add-to-load-path \"/no/such/directory/at/all\")"));
	CHECK(eval_error_contains(
	    "(require 'never-provided-anywhere)", "never-provided-anywhere"));
	CHECK(eval_eq("(+ 3 4)", "7"));

	kg_lisp_shutdown();
	remove_config_root(root);
}

static void test_math_natives(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(expt 2 8)"));
	CHECK(eval_ok("(expt 2 10)"));
	CHECK(eval_ok("(sin 0)"));
	CHECK(eval_ok("(cos 0)"));
	CHECK(eval_ok("(sqrt 16)"));
	CHECK(eval_ok("(log (exp 1))"));
	CHECK(eval_ok("(atan 1 1)"));

	CHECK(eval_ok("(floor 7 2)"));
	CHECK(eval_ok("(ceiling -7 2)"));
	CHECK(eval_ok("(truncate -7.5 2)"));

	CHECK(eval_eq("(round 2.5)", "2"));
	CHECK(eval_eq("(round -2.5)", "-2"));
	CHECK(eval_eq("(floor -7 2)", "-4"));

	/* Integer division by zero is an arith-error, as in Emacs; the
	 * float spelling (/ 1.0 0) is what produces the nonfinite values
	 * test_format_natives exercises. */
	CHECK(eval_error_contains("(/ 1 0)", "arith-error"));
	CHECK(eval_error_contains("(/ 0)", "arith-error"));
	CHECK(eval_error_contains("(/ 5 0)", "arith-error"));
	CHECK(eval_eq("(/ 1.0 0)", "1.0e+INF"));

	kg_lisp_shutdown();
}

static int eval_eq_int(const char *source, int expected)
{
	char want[32];

	(void)snprintf(want, sizeof(want), "%d", expected);
	return eval_eq(source, want);
}

/* "abc" / "héllo" / "漢字": 1-based codepoint positions are
 * a=1 b=2 c=3 \n=4 h=5 é=6 l=7 l=8 o=9 \n=10 漢=11 字=12, so
 * (point-max) is 13 even though the buffer holds 15 bytes. */
static void setup_utf8_buffer(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor_insert_row(bcur(), 1, "h\xc3\xa9llo", 6);
	editor_insert_row(bcur(), 2, "\xe6\xbc\xa2\xe5\xad\x97", 6);
}

static void test_point_offsets(void)
{
	char source[64];
	int n;

	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(point-min)", "1"));
	CHECK(eval_eq("(point-max)", "13"));

	/* Every position and count is an integer now, not a double. */
	CHECK(eval_eq("(type-of (point-min))", "integer"));
	CHECK(eval_eq("(type-of (point))", "integer"));
	CHECK(eval_eq("(type-of (point-max))", "integer"));
	CHECK(eval_eq("(type-of (line-number-at-pos))", "integer"));
	CHECK(eval_eq("(type-of (current-column))", "integer"));

	/* Every position round-trips, and offsets count codepoints even though
	 * the stored column is a byte index. */
	for (n = 1; n <= 13; n++) {
		(void)snprintf(source, sizeof(source), "(goto-char %d)", n);
		CHECK(eval_ok(source));
		CHECK(eval_eq_int("(point)", n));
	}
	/* Position 7 is the first "l" of "héllo", which the editor stores at
	 * byte column 3 of row 2. */
	CHECK(eval_ok("(goto-char 7)"));
	CHECK(eval_eq("(line-number-at-pos)", "2"));
	CHECK(eval_eq("(current-column)", "2"));
	CHECK(eval_ok("(goto-char 11)"));
	CHECK(eval_eq("(line-number-at-pos)", "3"));
	CHECK(eval_eq("(current-column)", "0"));

	/* Out-of-range positions clamp at both ends. */
	CHECK(eval_eq("(progn (goto-char -400) (point))", "1"));
	CHECK(eval_eq("(progn (goto-char 0) (point))", "1"));
	CHECK(eval_eq("(progn (goto-char 9999) (point))", "13"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_current_column_tab(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "\tx", 2);
	CHECK(kg_lisp_init() == 0);

	/* Display column, so the tab expands to the next tab stop (8),
	 * matching Emacs' current-column. */
	CHECK(eval_eq("(progn (goto-char 1) (current-column))", "0"));
	CHECK(eval_eq("(progn (goto-char 2) (current-column))", "8"));
	CHECK(eval_eq("(progn (goto-char 3) (current-column))", "9"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_char_after(void)
{
	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(char-after 1)", "97")); /* a */
	CHECK(eval_eq("(char-after 4)", "10")); /* row separator */
	CHECK(eval_eq("(char-after 6)", "233")); /* U+00E9 */
	CHECK(eval_eq("(char-after 11)", "28450")); /* U+6F22 */
	CHECK(eval_eq("(char-after 12)", "23383")); /* U+5B57 */
	CHECK(eval_eq("(char-after 13)", "nil")); /* end of buffer */
	CHECK(eval_eq("(char-after 9999)", "nil"));
	CHECK(eval_eq("(char-after 0)", "97"));
	CHECK(eval_eq("(progn (goto-char 6) (char-after))", "233"));
	CHECK(eval_eq("(progn (goto-char 6) (char-after nil))", "233"));
	CHECK(eval_eq("(progn (goto-char (point-max)) (char-after))", "nil"));
	/* A char code is an integer now, not a double. */
	CHECK(eval_eq("(type-of (char-after 1))", "integer"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_buffer_substring(void)
{
	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(buffer-substring 5 9)", "h\xc3\xa9ll"));
	/* Order-insensitive, like Emacs. */
	CHECK(eval_eq("(buffer-substring 9 5)", "h\xc3\xa9ll"));
	CHECK(eval_eq("(buffer-substring 3 3)", ""));
	CHECK(eval_eq("(buffer-substring 1 13)",
	    "abc\nh\xc3\xa9llo\n\xe6\xbc\xa2\xe5\xad\x97"));
	/* Both ends clamp. */
	CHECK(eval_eq("(buffer-substring -20 9999)",
	    "abc\nh\xc3\xa9llo\n\xe6\xbc\xa2\xe5\xad\x97"));
	CHECK(eval_error_contains("(buffer-substring 1)", "too few arguments"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_mark_and_region(void)
{
	int dirty_before;

	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);
	dirty_before = bcur()->dirty;

	/* No mark: (mark) is nil and the region accessors signal. */
	CHECK(eval_eq("(mark)", "nil"));
	CHECK(eval_error_contains("(region-beginning)", "no region"));
	CHECK(eval_error_contains("(region-end)", "no region"));

	CHECK(eval_ok("(set-mark 5)"));
	CHECK(kg_mark_is_set(bcur()));
	CHECK(bcur()->mark_highlight == 1);
	CHECK(eval_eq("(mark)", "5"));
	CHECK(eval_eq("(progn (goto-char 9) (region-beginning))", "5"));
	CHECK(eval_eq("(region-end)", "9"));
	CHECK(eval_eq("(buffer-substring (region-beginning) (region-end))",
	    "h\xc3\xa9ll"));

	/* Mark after point still yields an ordered region. */
	CHECK(eval_ok("(progn (set-mark 9) (goto-char 5))"));
	CHECK(eval_eq("(region-beginning)", "5"));
	CHECK(eval_eq("(region-end)", "9"));

	/* Out-of-range marks clamp. */
	CHECK(eval_ok("(set-mark 9999)"));
	CHECK(eval_eq("(mark)", "13"));

	/* deactivate-mark drops the highlight but keeps the mark. */
	CHECK(eval_ok("(deactivate-mark)"));
	CHECK(bcur()->mark_highlight == 0);
	CHECK(kg_mark_is_set(bcur()));
	CHECK(eval_eq("(mark)", "13"));

	/* None of this touches the buffer. */
	CHECK(bcur()->dirty == dirty_before);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_word_motion(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one two three", 13);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 1) (forward-word) (point))", "4"));
	CHECK(eval_eq("(progn (goto-char 1) (forward-word 2) (point))", "8"));
	/* A huge count stops at the edge instead of spinning. */
	CHECK(
	    eval_eq("(progn (goto-char 1) (forward-word 9999) (point))", "14"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word) (point))", "9"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word 9999) (point))",
	    "1"));
	/* Negative counts reverse direction, as in Emacs. */
	CHECK(eval_eq("(progn (goto-char 1) (backward-word -1) (point))", "4"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (forward-word -1) (point))", "9"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* forward-word/backward-word cross row boundaries treating a row break --
 * even one crossing an empty row -- as a single separator, never a stop,
 * matching src/word.c's word_boundary_forward().  This also pins the
 * O(rows)-per-scanned-byte regression: the buggy lisp_flat_byte_at()
 * (since removed) re-derived (row, col) from a flat position on every
 * scanned byte, which this multi-row case exercises even though the test
 * itself only checks the result, not the cost. */
static void test_word_motion_crosses_rows(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one", 3);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, "two three", 9);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 1) (forward-word) (point))", "4"));
	CHECK(eval_eq("(progn (goto-char 1) (forward-word 2) (point))", "9"));
	CHECK(eval_eq("(progn (goto-char 1) (forward-word 3) (point))", "15"));
	CHECK(eval_eq("(point-max)", "15"));

	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word) (point))", "10"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word 2) (point))", "6"));
	CHECK(eval_eq(
	    "(progn (goto-char (point-max)) (backward-word 3) (point))", "1"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_editor_bridge(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "abc", 3);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(message \"bridged\")"));
	CHECK(strcmp(test_status_message, "bridged") == 0);
	CHECK(eval_eq("(buffer-name)", "bridge.txt"));
	CHECK(eval_ok("(progn (goto-char 1) (insert \"z\"))"));
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", "zabc"));
	CHECK(eval_ok(
	    "(define-command \"greet\" (lambda () (message \"hey\")))"));
	CHECK(eval_ok("(global-set-key \"C-c i\" \"greet\")"));
	CHECK(keybind_lookup("C-c i") != nullptr);
	CHECK(eval_ok("(global-unset-key \"C-c i\")"));
	CHECK(keybind_lookup("C-c i") == nullptr);
	CHECK(
	    eval_error_contains("(load \"absent-package\")", "absent-package"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* ---- Buffer objects and the runtime execution context ---------------- */

/* Buffer undo depth, for the hidden-edit isolation checks below. */
static int undo_depth_of_buffer(struct editor_buffer *b)
{
	return b ? b->undostack.size : -1;
}

static void test_buffer_objects(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* current-buffer answers with the display buffer, an adapter object
	 * that is eq-identical across asks for the same buffer. */
	CHECK(eval_eq("(type-of (current-buffer))", "buffer"));
	CHECK(eval_eq("(buffer-live-p (current-buffer))", "t"));
	CHECK(eval_eq("(buffer-name)", "bridge.txt"));
	CHECK(
	    eval_eq("(eq (current-buffer) (get-buffer \"bridge.txt\"))", "t"));
	CHECK(eval_eq("(get-buffer \"absent\")", "nil"));
	CHECK(eval_eq("(buffer-live-p 42)", "nil"));
	CHECK(eval_error_contains("(buffer-name 42)", "expected a buffer"));

	/* get-buffer-create makes a clean hidden buffer, idempotent by name. */
	CHECK(eval_ok("(setq h (get-buffer-create \"hidden\"))"));
	CHECK(eval_eq("(buffer-name h)", "hidden"));
	CHECK(eval_eq("(eq h (get-buffer-create \"hidden\"))", "t"));
	CHECK(eval_eq("(buffer-live-p h)", "t"));

	/* buffer-list: current buffer first, every live buffer exactly once,
	 * the just-created hidden buffer present. */
	CHECK(eval_eq("(length (buffer-list))", "2"));
	CHECK(eval_eq("(eq (car (buffer-list)) (current-buffer))", "t"));
	CHECK(eval_eq("(if (memq h (buffer-list)) t nil)", "t"));
	CHECK(eval_eq(
	    "(if (memq (current-buffer) (cdr (buffer-list))) t nil)", "nil"));

	/* Hidden-buffer editing: insert lands there and the displayed
	 * window's point does not move; the edit is one undo record in the
	 * hidden buffer and none in the display buffer's chain. */
	CHECK(eval_eq("(progn (set-buffer h) (insert \"hello\")"
		      " (list (buffer-name) (point)"
		      " (buffer-substring (point-min) (point-max))))",
	    "(\"hidden\" 6 \"hello\")"));
	CHECK(wcur()->cx == 0 && wcur()->cy == 0);
	CHECK(bcur()->undostack.size == 0);
	CHECK(undo_depth_of_buffer(buf_resolve(buf_handle(1))) == 1);
	/* A second hidden edit is a second undo record. */
	CHECK(eval_ok("(progn (set-buffer h) (insert \"x\"))"));
	CHECK(undo_depth_of_buffer(buf_resolve(buf_handle(1))) == 2);
	/* The display buffer is untouched. */
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", ""));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_kill_buffer(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Killing a hidden buffer leaves its object dead. */
	CHECK(eval_ok("(setq h (get-buffer-create \"doomed\"))"));
	CHECK(eval_eq("(kill-buffer h)", "nil"));
	CHECK(eval_eq("(buffer-live-p h)", "nil"));
	CHECK(eval_error_contains("(buffer-name h)", "buffer is dead"));

	/* A modified buffer is refused without prompting. */
	CHECK(eval_ok("(setq h (get-buffer-create \"dirty\"))"));
	CHECK(eval_ok("(progn (set-buffer h) (insert \"x\"))"));
	CHECK(eval_error_contains("(kill-buffer h)", "modified"));
	CHECK(eval_eq("(buffer-live-p h)", "t"));

	/* Killing the exec buffer leaves later work on it an error. */
	CHECK(eval_ok("(setq h (get-buffer-create \"current\"))"));
	CHECK(eval_error_contains(
	    "(progn (set-buffer h) (kill-buffer (current-buffer)) (point))",
	    "current buffer is dead"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_buffer_stale_slot(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Kill a buffer, create another in the same slot; the old object
	 * must not alias the new buffer. */
	CHECK(eval_ok("(insert \"KEEP\")"));
	CHECK(eval_ok("(setq a (get-buffer-create \"slot-a\"))"));
	CHECK(eval_ok("(kill-buffer a)"));
	CHECK(eval_ok("(setq b (get-buffer-create \"slot-b\"))"));
	CHECK(eval_eq("(buffer-name b)", "slot-b"));
	CHECK(eval_eq("(buffer-live-p a)", "nil"));
	CHECK(eval_error_contains("(buffer-name a)", "buffer is dead"));
	CHECK(eval_error_contains("(set-buffer a)", "buffer is dead"));
	/* The display buffer's text is untouched by the slot reuse. */
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", "KEEP"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_buffer_point_sync(void)
{
	char result[128] = "";
	setup_editor();
	editor_insert_row(bcur(), 0, "one two", 7);
	CHECK(kg_lisp_init() == 0);

	/* A successful goto-char syncs the runtime point to the window. */
	CHECK(kg_lisp_eval_string("(goto-char 3)", sizeof("(goto-char 3)") - 1,
		  result, sizeof(result))
	    == 0);
	CHECK(wcur()->cx == 2);
	CHECK(wcur()->cy == 0);
	CHECK(eval_eq("(point)", "3"));

	/* A failed frame must not synchronize: the window stays put. */
	CHECK(eval_error_contains("(progn (goto-char 7) (car 1))", "pair"));
	CHECK(wcur()->cx == 2);
	CHECK(wcur()->cy == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

/* lisp_finite() is the seam every numeric argument of a kg native crosses,
 * and it used to hand the type check to FeToDouble, whose failure text is
 * "expected double, got string" -- fe's own wording, naming neither Emacs'
 * condition nor either of kg's two number types since 05D.  It now tags the
 * operand itself and raises Emacs' wrong-type-argument, message-level for
 * now (Phase 6 makes it a structured condition).  A NaN passes the tag test
 * and is still refused on its value, with its own text; nothing about the
 * accepted values moved. */
static void test_numeric_argument_seam(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one two", 7);
	CHECK(kg_lisp_init() == 0);

	/* Every native that reads a position or a count through the seam. */
	CHECK(eval_error_contains("(goto-char \"x\")", "wrong-type-argument"));
	CHECK(eval_error_contains("(goto-line \"x\")", "wrong-type-argument"));
	CHECK(
	    eval_error_contains("(forward-word \"x\")", "wrong-type-argument"));
	CHECK(eval_error_contains(
	    "(char-to-string \"x\")", "wrong-type-argument"));
	CHECK(eval_error_contains(
	    "(substring \"abcd\" \"1\" 2)", "wrong-type-argument"));
	/* Not only strings: a symbol, a list, nil and an adapter object are
	 * the same verdict, where each used to name a raw fe type tag. */
	CHECK(eval_error_contains("(goto-char 'a)", "wrong-type-argument"));
	CHECK(
	    eval_error_contains("(goto-char (list 1))", "wrong-type-argument"));
	CHECK(eval_error_contains("(goto-char nil)", "wrong-type-argument"));
	CHECK(eval_error_contains(
	    "(goto-char (make-marker))", "wrong-type-argument"));
	/* The retired text is gone, not merely joined. */
	CHECK(eval_error_contains("(goto-char \"x\")", "wrong-type-argument"));
	CHECK(!eval_error_contains("(goto-char \"x\")", "expected double"));

	/* A NaN keeps its own message, and an infinity still clamps rather
	 * than raising -- both unchanged by the tag check in front. */
	CHECK(
	    eval_error_contains("(goto-char (- (/ 1.0 0) (/ 1.0 0)))", "NaN"));
	CHECK(eval_ok("(goto-char (/ 1.0 0))"));

	/* Both number tags are accepted, and an integer still widens. */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_eq("(point)", "3"));
	CHECK(eval_ok("(goto-char 5.0)"));
	CHECK(eval_eq("(point)", "5"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Point is a property of the buffer, not of the frame that happens to be
 * looking at it: (goto-char N), set-buffer away to a hidden buffer and
 * straight back within the same frame, must still see N.  Native-test
 * half of the PTY case lisp-point-survives-set-buffer.yaml. */
static void test_point_survives_set_buffer(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 4)"
		      " (set-buffer (get-buffer-create \"scratch\"))"
		      " (set-buffer (get-buffer \"bridge.txt\"))"
		      " (point))",
	    "4"));
	CHECK(eval_ok("(insert \"!\")"));
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", "abc!def"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* A hidden buffer's runtime point outlives the frame that created it: a
 * later, separate frame that set-buffers to it must pick up point where
 * the earlier frame's edit left it, not at point-min.  Native-test half
 * of the PTY case lisp-hidden-buffer-point-persists.yaml. */
static void test_hidden_buffer_point_persists_across_frames(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(progn (set-buffer (get-buffer-create \"scratch\"))"
		      " (insert \"hello\"))"));
	CHECK(eval_eq("(progn (set-buffer (get-buffer \"scratch\"))"
		      " (insert \"X\")"
		      " (buffer-substring (point-min) (point-max)))",
	    "helloX"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Frame entry is authoritative for the buffer the active window shows: it
 * overwrites that buffer's point-table entry from the window cursor on
 * every frame, rather than keeping whatever an earlier frame (or a stale
 * table entry) left there -- the window may have moved between commands
 * without going through Lisp at all. */
static void test_frame_entry_overwrites_from_window_cursor(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one two three", 13);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(progn (goto-char 9) (point))", "9"));
	wcur()->cy = 0;
	wcur()->cx = 0;
	CHECK(eval_eq("(point)", "1"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The adapter object pool is bounded: distinct buffer handles each take
 * one record, so enough create/kill cycles without releasing the Lisp
 * values exhaust it, and the exhaustion is an error rather than a silent
 * alias to an earlier record.  The loop keeps every object alive (the
 * `held` list), so Fe's GC cannot release any record behind it. */
static void test_buffer_object_capacity(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* Take the display buffer's object first: its record is what must
	 * survive the exhaustion below. */
	CHECK(eval_ok("(setq before (current-buffer))"));

	/* The adapter object pool is bounded: distinct buffer handles each
	 * take one record, so enough create/kill cycles while every Lisp
	 * value stays alive exhaust it, and the exhaustion is an error
	 * rather than a silent alias to an earlier record. */
	CHECK(eval_error_contains("(progn (setq held '()) (setq n 0)"
				  " (while (< n 100)"
				  "  (setq held (cons (get-buffer-create "
				  "(format \"cap-%d\" n)) held))"
				  "  (kill-buffer (car held))"
				  "  (setq n (+ n 1))))",
	    "too many buffer objects"));

	/* Exhaustion does not break existing objects: the display buffer's
	 * record was taken before the pool filled, so it still resolves and
	 * is still the same object a fresh ask returns (the pool is
	 * deduplicated per handle, not per ask). */
	CHECK(eval_eq("(buffer-live-p before)", "t"));
	CHECK(eval_eq("(eq before (current-buffer))", "t"));
	CHECK(eval_eq("(buffer-name before)", "bridge.txt"));
	/* Dropping the held list makes the wrappers unreachable again, so
	 * the pool has room for new distinct handles the next time Fe's GC
	 * actually collects. */
	CHECK(eval_ok("(setq held '())"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* ---- Editing natives: delete-region / replace-region ------------------ */

static void test_delete_region(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* Order-insensitive, like buffer-substring: (delete-region 8 3)
	 * deletes the same span as (delete-region 3 8). */
	CHECK(eval_ok("(delete-region 8 3)"));
	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "heorld", 6) == 0);
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	CHECK(bcur()->row[0].size == 11);
	CHECK(memcmp(bcur()->row[0].chars, "hello world", 11) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_delete_region_utf8(void)
{
	setup_editor();
	/* "caf\xC3\xA9" is 4 codepoints ('c', 'a', 'f', e-acute) in 5
	 * bytes; deleting codepoint 4 removes both bytes of the e-acute
	 * and nothing else. */
	editor_insert_row(bcur(), 0, "caf\xC3\xA9", 5);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(delete-region 4 5)"));
	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "caf", 3) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_delete_region_malformed_byte(void)
{
	setup_editor();
	/* A malformed lead byte counts as one codepoint of its own (see
	 * utf8_glyph_span_at() in def.h): "a\xFFb" is 3 codepoints in 3
	 * bytes, and deleting codepoint 2 removes exactly the bad byte. */
	editor_insert_row(bcur(), 0,
	    "a\xFF"
	    "b",
	    3);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(delete-region 2 3)"));
	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "ab", 2) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_delete_region_read_only_recovery(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(bcur(), 0, "original", 8);
	bcur()->readonly = 1;
	CHECK(kg_lisp_init() == 0);

	CHECK(kg_lisp_eval_string(
		  "(delete-region 1 4)", 20, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "buffer is read-only") != nullptr);
	/* Refused before any partial work: not one byte moved. */
	CHECK(bcur()->row[0].size == 8);
	CHECK(memcmp(bcur()->row[0].chars, "original", 8) == 0);
	CHECK(bcur()->undostack.size == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_replace_region(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(replace-region 1 6 \"goodbye\")"));
	CHECK(bcur()->row[0].size == 13);
	CHECK(memcmp(bcur()->row[0].chars, "goodbye world", 13) == 0);
	/* One gateway call, one undo step -- never a delete plus an
	 * insert. */
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	CHECK(bcur()->row[0].size == 11);
	CHECK(memcmp(bcur()->row[0].chars, "hello world", 11) == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_replace_region_read_only_recovery(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(bcur(), 0, "original", 8);
	bcur()->readonly = 1;
	CHECK(kg_lisp_init() == 0);

	CHECK(kg_lisp_eval_string(
		  "(replace-region 1 4 \"new\")", 27, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "buffer is read-only") != nullptr);
	CHECK(bcur()->row[0].size == 8);
	CHECK(memcmp(bcur()->row[0].chars, "original", 8) == 0);
	CHECK(bcur()->undostack.size == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

/* kg_edit_fail_alloc_after() is the seam test_buffer.c's own
 * refusal-is-atomic sweep uses to prove kg_buffer_replace() never
 * publishes a partial edit; this is the thin Lisp wrapper's half of that
 * proof, since neither native checks the gateway's return value (see
 * native_insert(), which this follows). */
static void test_replace_region_alloc_failure_is_atomic(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "alpha beta gamma", 17);
	CHECK(kg_lisp_init() == 0);

	kg_edit_fail_alloc_after(0);
	CHECK(eval_ok("(replace-region 7 11 \"BETA\")"));
	kg_edit_fail_alloc_after(-1);

	CHECK(bcur()->row[0].size == 17);
	CHECK(memcmp(bcur()->row[0].chars, "alpha beta gamma", 17) == 0);
	CHECK(bcur()->undostack.size == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

/* ---- Search natives ---------------------------------------------------- */

static void test_search_forward_backward(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "the cat sat on the mat", 22);
	CHECK(kg_lisp_init() == 0);

	/* search-forward moves point to the end of the match and returns
	 * the new point, as Emacs' does. */
	CHECK(eval_ok("(goto-char (point-min))"));
	CHECK(eval_eq("(search-forward \"cat\")", "8"));
	CHECK(eval_eq("(point)", "8"));
	/* No second "cat" ahead of point: nil, and point does not move. */
	CHECK(eval_eq("(search-forward \"cat\")", "nil"));
	CHECK(eval_eq("(point)", "8"));

	/* search-backward moves to the start of the match. */
	CHECK(eval_ok("(goto-char (point-max))"));
	CHECK(eval_eq("(search-backward \"at\")", "21"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_search_forward_bound(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "aaa bbb aaa", 11);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char (point-min))"));
	/* A bound before the second "aaa" hides it. */
	CHECK(eval_eq("(search-forward \"aaa\" 9)", "4"));
	CHECK(eval_eq("(search-forward \"aaa\" 9)", "nil"));
	CHECK(eval_eq("(point)", "4"));
	/* Without a bound the second occurrence is reachable. */
	CHECK(eval_eq("(search-forward \"aaa\")", "12"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Fe's string reader drops a lone backslash before a non-escape character
 * ('\n', '\r' and '\t' are the only real escapes; see Read()'s '"' case
 * in fe.c), so a literal backslash inside a Fe string literal takes two
 * source backslashes -- exactly like Emacs Lisp's own string syntax.
 * `pattern` is written the way kg_regex_compile() sees it, one backslash
 * per escape, e.g. "\\(a*\\)*b"; this doubles each of its backslashes
 * before splicing it into `fmt`'s one %s, producing the Fe source that
 * reads back to that same pattern -- so a test does not have to work out
 * the double escaping by hand. */
static void fe_regex_source(
    char *out, size_t outsize, const char *fmt, const char *pattern)
{
	char quoted[200];
	size_t i = 0, o = 0;

	while (pattern[i] != '\0' && o + 2 < sizeof(quoted)) {
		if (pattern[i] == '\\') {
			quoted[o++] = '\\';
		}
		quoted[o++] = pattern[i++];
	}
	quoted[o] = '\0';
	(void)snprintf(out, outsize, fmt, quoted);
}

static void test_re_search_and_match_data(void)
{
	char source[256];

	setup_editor();
	editor_insert_row(bcur(), 0, "foobar", 6);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char (point-min))"));
	fe_regex_source(source, sizeof(source), "(re-search-forward \"%s\")",
	    "\\(foo\\)bar");
	CHECK(eval_eq(source, "7"));
	CHECK(eval_eq("(match-beginning 0)", "1"));
	CHECK(eval_eq("(match-end 0)", "7"));
	CHECK(eval_eq("(match-beginning 1)", "1"));
	CHECK(eval_eq("(match-end 1)", "4"));
	/* Group 2 was never in the pattern. */
	CHECK(eval_eq("(match-beginning 2)", "nil"));
	CHECK(eval_eq("(match-end 2)", "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* re-search-backward: two possible matches on the row, searching from
 * point-max must pick the later one (kg_regex_match_backward()'s own
 * "last match ending at or before the limit" policy, src/regex.c), and a
 * no-match search afterwards must not disturb point or the previous
 * match data -- lisp_search() only writes state.match and moves the
 * marker on a found match. */
static void test_re_search_backward(void)
{
	char source[256];

	setup_editor();
	editor_insert_row(bcur(), 0, "foobar foobar", 13);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char (point-max))"));
	fe_regex_source(source, sizeof(source), "(re-search-backward \"%s\")",
	    "\\(foo\\)bar");
	/* The second "foobar" (0-based [7,13)), not the first: the closer
	 * match to the starting point wins. */
	CHECK(eval_eq(source, "8"));
	CHECK(eval_eq("(point)", "8"));
	CHECK(eval_eq("(match-beginning 0)", "8"));
	CHECK(eval_eq("(match-end 0)", "14"));
	CHECK(eval_eq("(match-beginning 1)", "8"));
	CHECK(eval_eq("(match-end 1)", "11"));

	/* No match: nil, and point plus the previous match data are left
	 * exactly as the successful search above left them. */
	fe_regex_source(
	    source, sizeof(source), "(re-search-backward \"%s\")", "zzz");
	CHECK(eval_eq(source, "nil"));
	CHECK(eval_eq("(point)", "8"));
	CHECK(eval_eq("(match-beginning 0)", "8"));
	CHECK(eval_eq("(match-end 0)", "14"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_regex_too_complex_and_bad_pattern(void)
{
	char source[256];

	setup_editor();
	/* 24 "a"s with no "b": the classic catastrophic-backtracking shape
	 * for "\(a*\)*b" trips this engine's step budget -- see
	 * test_regex.c's test_exhausted_budget_is_not_no_match(), which
	 * pins the exact boundary this reuses.  Too-complex is a distinct,
	 * truthful error, never folded into "no match". */
	editor_insert_row(bcur(), 0, "aaaaaaaaaaaaaaaaaaaaaaaa", 24);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char (point-min))"));
	fe_regex_source(
	    source, sizeof(source), "(re-search-forward \"%s\")", "\\(a*\\)*b");
	CHECK(eval_error_contains(source, "too complex"));

	/* An unclosed group is a bad pattern: a distinct error again, not
	 * silently treated as "no match" either. */
	fe_regex_source(source, sizeof(source), "(re-search-forward \"%s\")",
	    "\\(unclosed");
	CHECK(eval_error_contains(source, "invalid regexp"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_search_cancellation(void)
{
	char result[128] = "";
	const char *source = "(search-forward \"zzz\")";

	setup_editor();
	editor_insert_row(bcur(), 0, "xxxx", 4);
	editor_insert_row(bcur(), 1, "xxxx", 4);
	editor_insert_row(bcur(), 2, "xxxx", 4);
	CHECK(kg_lisp_init() == 0);
	CHECK(eval_ok("(goto-char (point-min))"));

	/* C-g reaches a multi-row search through the same interrupt check
	 * every long-running native polls; the row loop checks it once per
	 * row; two non-matching rows are enough to trip a threshold of 2. */
	interrupt_polls = 0;
	kg_lisp_set_interrupt_check(cancel_evaluation);
	CHECK(
	    kg_lisp_eval_string(source, strlen(source), result, sizeof(result))
	    != 0);
	CHECK(strcmp(result, "Quit") == 0);
	kg_lisp_set_interrupt_check(nullptr);

	/* Cancellation is distinct from "not found": an uninterrupted retry
	 * reports nil, not another error. */
	CHECK(eval_eq(source, "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* ---- Marker natives ----------------------------------------------------
 * (make-marker) here starts at point in the exec buffer, per this
 * sub-plan's own table -- not Emacs' make-marker, which starts detached
 * (Emacs' point-marker is the one that starts at point).  Recorded as a
 * naming mismatch worth revisiting in the Phase 3 notes. */

static void test_markers(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char 7)"));
	CHECK(eval_ok("(setq m (make-marker))"));
	CHECK(eval_eq("(type-of m)", "marker"));
	CHECK(eval_eq("(marker-position m)", "7"));
	CHECK(eval_eq("(eq (marker-buffer m) (current-buffer))", "t"));
	/* Unlike buffer objects, two markers are never eq. */
	CHECK(eval_eq("(eq (make-marker) (make-marker))", "nil"));

	/* An insertion strictly before the marker always pushes it
	 * forward, whatever its gravity. */
	CHECK(eval_ok("(goto-char 1)"));
	CHECK(eval_ok("(insert \"XXXXX\")"));
	CHECK(eval_eq("(marker-position m)", "12"));

	/* set-marker moves the same marker object, including to a
	 * different buffer, and returns it.  set-buffer only lasts for the
	 * top-level form that calls it (see the struct kg_lisp_exec_ctx
	 * comment in lisp_internal.h), so the buffer switch and the insert
	 * it enables have to be one form. */
	CHECK(eval_ok("(setq h (get-buffer-create \"scratch\"))"));
	CHECK(eval_ok("(progn (set-buffer h) (insert \"abcdef\"))"));
	CHECK(eval_eq("(eq (set-marker m 3 h) m)", "t"));
	CHECK(eval_eq("(marker-position m)", "3"));
	CHECK(eval_eq("(eq (marker-buffer m) h)", "t"));

	/* A nil position detaches it: no position, no buffer. */
	CHECK(eval_ok("(set-marker m nil)"));
	CHECK(eval_eq("(marker-position m)", "nil"));
	CHECK(eval_eq("(marker-buffer m)", "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* A marker outlives the buffer it named -- not an error, unlike a dead
 * buffer object -- and set-marker on one whose old buffer is gone still
 * works, by simply creating a fresh underlying marker in the new
 * buffer. */
static void test_marker_survives_buffer_kill(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq h (get-buffer-create \"doomed\"))"));
	/* set-buffer and make-marker have to share a form for the marker to
	 * land in "doomed" rather than the window's own buffer -- see the
	 * struct kg_lisp_exec_ctx comment in lisp_internal.h. */
	CHECK(eval_ok("(progn (set-buffer h) (setq m (make-marker)))"));
	CHECK(eval_ok("(kill-buffer h)"));

	CHECK(eval_eq("(marker-position m)", "nil"));
	CHECK(eval_eq("(marker-buffer m)", "nil"));

	CHECK(eval_ok("(set-marker m 1)"));
	CHECK(eval_eq("(marker-position m)", "1"));
	CHECK(eval_eq("(eq (marker-buffer m) (current-buffer))", "t"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Indices count codepoints, so "héllo" is 5 characters in 6 bytes and
 * "漢字" is 2 characters in 6 bytes. */
static void test_string_length_and_substring(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(string-length \"\")", "0"));
	CHECK(eval_eq("(string-length \"abc\")", "3"));
	CHECK(eval_eq("(string-length \"h\xc3\xa9llo\")", "5"));
	CHECK(eval_eq("(string-length \"\xe6\xbc\xa2\xe5\xad\x97\")", "2"));
	CHECK(eval_error_contains("(string-length 1)", "expected string"));
	/* Lengths and char codes are integers now, not doubles. */
	CHECK(eval_eq("(type-of (string-length \"abc\"))", "integer"));
	CHECK(eval_eq("(type-of (string-to-char \"abc\"))", "integer"));

	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" 1)", "\xc3\xa9llo"));
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" 1 3)", "\xc3\xa9l"));
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" 0 2)", "h\xc3\xa9"));
	CHECK(eval_eq(
	    "(substring \"\xe6\xbc\xa2\xe5\xad\x97\" 1)", "\xe5\xad\x97"));
	CHECK(eval_eq(
	    "(substring \"\xe6\xbc\xa2\xe5\xad\x97\" 0 1)", "\xe6\xbc\xa2"));
	/* Negative indices count from the end, as in Emacs. */
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" -2)", "lo"));
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" 1 -1)", "\xc3\xa9ll"));
	CHECK(eval_eq("(substring \"h\xc3\xa9llo\" -4 -3)", "\xc3\xa9"));
	/* nil means "the default end". */
	CHECK(eval_eq("(substring \"abc\" nil 2)", "ab"));
	CHECK(eval_eq("(substring \"abc\" 1 nil)", "bc"));
	/* Out of range clamps at both ends; a reversed range is empty. */
	CHECK(eval_eq("(substring \"abc\" 9)", ""));
	CHECK(eval_eq("(substring \"abc\" 0 99)", "abc"));
	CHECK(eval_eq("(substring \"abc\" -99)", "abc"));
	CHECK(eval_eq("(substring \"abc\" 2 1)", ""));
	CHECK(eval_eq("(substring \"abc\" 3 3)", ""));
	CHECK(eval_error_contains("(substring \"abc\")", "too few arguments"));

	kg_lisp_shutdown();
}

static void test_string_concat_and_equal(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(concat)", ""));
	CHECK(eval_eq("(concat \"a\")", "a"));
	CHECK(eval_eq("(concat \"a\" \"\" \"bc\" \"d\")", "abcd"));
	CHECK(eval_eq("(concat \"\xe6\xbc\xa2\" \"\xe5\xad\x97\")",
	    "\xe6\xbc\xa2\xe5\xad\x97"));
	CHECK(eval_eq("(string-length (concat \"h\xc3\xa9\" \"llo\"))", "5"));
	CHECK(eval_error_contains("(concat \"a\" 1)", "expected string"));

	CHECK(eval_eq("(string= \"\" \"\")", "t"));
	CHECK(eval_eq("(string= \"abc\" \"abc\")", "t"));
	CHECK(eval_eq("(string= \"abc\" \"abd\")", "nil"));
	CHECK(eval_eq("(string= \"abc\" \"ab\")", "nil"));
	CHECK(eval_eq("(string= \"h\xc3\xa9llo\" \"h\xc3\xa9llo\")", "t"));
	CHECK(eval_eq("(string= \"h\xc3\xa9llo\" \"hello\")", "nil"));
	/* Recovery after a type error leaves the interpreter usable. */
	CHECK(eval_error_contains("(string= \"a\" 1)", "expected string"));
	CHECK(eval_eq("(concat \"ok\")", "ok"));

	kg_lisp_shutdown();
}

/* Every expectation here was measured against Emacs 31, except the two
 * noted places where kg has no bignums and no Emacs behaviour to copy. */
static void test_format_natives(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(format \"\")", ""));
	CHECK(eval_eq("(format \"plain\")", "plain"));
	CHECK(eval_eq("(format \"100%%\")", "100%"));

	/* %s and %S are fe's printer with the quoting flag flipped. */
	CHECK(eval_eq("(format \"%s\" 42)", "42"));
	CHECK(eval_eq("(format \"%s\" 42.5)", "42.5"));
	CHECK(eval_eq("(format \"%s\" \"hi\")", "hi"));
	CHECK(eval_eq("(format \"%S\" \"hi\")", "\"hi\""));
	CHECK(eval_eq("(format \"%s\" 'foo)", "foo"));
	CHECK(eval_eq("(format \"%s\" nil)", "nil"));
	CHECK(eval_eq("(format \"%s\" (list 1 2))", "(1 2)"));
	CHECK(eval_eq("(format \"%S\" (list 1 \"a\"))", "(1 \"a\")"));
	CHECK(eval_eq("(format \"%s=%S\" 'a \"b\")", "a=\"b\""));
	CHECK(eval_eq("(format \"h\xc3\xa9llo %s\" \"w\xc3\xb6rld\")",
	    "h\xc3\xa9llo w\xc3\xb6rld"));

	/* %d truncates toward zero. */
	CHECK(eval_eq("(format \"%d\" 42)", "42"));
	CHECK(eval_eq("(format \"%d\" 42.9)", "42"));
	CHECK(eval_eq("(format \"%d\" -3.7)", "-3"));
	CHECK(eval_eq("(format \"%d\" -0.5)", "0"));
	/* An integer argument prints its own value exactly -- the int64 path
	 * bypasses the double, so 2^53 + 1 is no longer rounded on the way
	 * in.  The double fast path's guard keeps the cast defined; the wide
	 * float values are exact, as they are in Emacs. */
	CHECK(eval_eq("(format \"%d\" 9007199254740993)", "9007199254740993"));
	CHECK(eval_eq("(format \"%d\" 1e19)", "10000000000000000000"));
	CHECK(eval_eq("(format \"%d\" -1e19)", "-10000000000000000000"));
	CHECK(eval_eq("(string-length (format \"%d\" 1e300))", "301"));
	CHECK(eval_eq("(substring (format \"%d\" 1e300) 0 4)", "1000"));
	/* kg has no bignums to print NaN or an infinity into, so unlike
	 * Emacs, which writes "nan" and "inf", %d refuses them.  The float
	 * is spelled (/ 1.0 0): (/ 1 0) is integer division by zero and an
	 * arith-error, so it never reaches format at all. */
	CHECK(eval_error_contains("(format \"%d\" (/ 1.0 0))", "finite"));
	CHECK(eval_error_contains(
	    "(format \"%d\" (- (/ 1.0 0) (/ 1.0 0)))", "finite"));
	/* %s and %S are fe's printer, so the exceptional floats now wear
	 * fe's readable spelling, the same "1.0e+INF"/"-0.0e+NaN" family
	 * Emacs writes; the C-style "inf" is what %d and the float
	 * conversions below produce. */
	CHECK(eval_eq("(format \"%s\" (/ 1.0 0))", "1.0e+INF"));
	CHECK(eval_eq("(format \"%S\" (/ 1.0 0))", "1.0e+INF"));
	CHECK(eval_eq("(format \"%s\" (- 0 (/ 1.0 0)))", "-1.0e+INF"));

	/* %e, %f and %g are C's conversions, which is exactly what Emacs
	 * uses; every one of these was checked against Emacs 31.  An integer
	 * argument widens through FeToDouble, so (format "%e" 42) formats
	 * the way it does in Emacs. */
	CHECK(eval_eq("(format \"%e\" 1.5)", "1.500000e+00"));
	CHECK(eval_eq("(format \"%e\" 42)", "4.200000e+01"));
	CHECK(eval_eq("(format \"%e\" 1e300)", "1.000000e+300"));
	CHECK(eval_eq("(format \"%f\" 1.5)", "1.500000"));
	CHECK(eval_eq("(format \"%f\" -2.25)", "-2.250000"));
	CHECK(eval_eq("(format \"%f\" 42)", "42.000000"));
	CHECK(eval_eq("(format \"%g\" 1.5)", "1.5"));
	CHECK(eval_eq("(format \"%g\" 0)", "0"));
	CHECK(eval_eq("(format \"%g\" 1e300)", "1e+300"));
	CHECK(eval_eq("(format \"%-5d|\" 3)", "3    |"));
	CHECK(eval_eq("(format \"%05.2f\" 1.5)", "01.50"));
	CHECK(eval_eq("(format \"%05d\" -3)", "-0003"));
	CHECK(eval_eq("(format \"%05.2d\" -3)", "  -03"));
	CHECK(eval_eq("(format \"%c\" 65)", "A"));
	CHECK(eval_eq("(format \"%c\" 233)", "\xc3\xa9"));
	CHECK(eval_eq("(format \"%c\" 128169)", "\xf0\x9f\x92\xa9"));
	CHECK(eval_eq("(format \"%x %X %o\" 255 255 15)", "ff FF 17"));
	CHECK(eval_eq("(format \"%x %X %o\" -15 -15 -15)", "-f -F -17"));
	CHECK(eval_eq("(format \"%-5s|\" \"x\")", "x    |"));
	CHECK(eval_eq("(format \"%.3s\" \"hello\")", "hel"));
	CHECK(eval_eq("(format \"%3.1s\" \"\xc3\xa9x\")", "  \xc3\xa9"));
	CHECK(eval_eq("(format \"%-3.1s\" \"\xc3\xa9x\")", "\xc3\xa9  "));
	CHECK(eval_error_contains(
	    "(format \"%999999999999d\" 1)", "field is too large"));
	/* Unlike %d, the float conversions have a rendering for the
	 * exceptional values, so they print them instead of raising. */
	CHECK(eval_eq("(format \"%e\" (/ 1.0 0))", "inf"));
	CHECK(eval_eq("(format \"%f\" (- 0 (/ 1.0 0)))", "-inf"));
	CHECK(eval_eq("(format \"%g\" (/ 1.0 0))", "inf"));
	CHECK(eval_error_contains("(format \"%f\" \"x\")", "argument type"));
	/* %f of DBL_MAX is the longest thing the buffer must hold. */
	CHECK(eval_eq("(string-length (format \"%f\" 1e300))", "308"));

	/* Extra arguments are ignored; missing ones are an error. */
	CHECK(eval_eq("(format \"%s\" 1 2)", "1"));
	CHECK(eval_error_contains("(format \"%s %s\" 1)", "not enough"));
	CHECK(eval_error_contains("(format \"%s\")", "not enough"));
	CHECK(eval_error_contains("(format \"%d\" \"x\")", "argument type"));
	CHECK(eval_error_contains(
	    "(format \"%q\" 1)", "invalid format operation %q"));
	CHECK(eval_error_contains("(format \"50%\")", "middle of format"));
	CHECK(eval_error_contains("(format 1)", "expected string"));
	/* The spellings Emacs accepts and kg refuses, recorded rather than
	 * quietly misread (manifest row phase8-format-strictness): Emacs'
	 * remaining flags and its N$ field numbers, and %c of 0, which
	 * Emacs writes as a NUL byte and kg cannot store in a string. */
	CHECK(eval_error_contains(
	    "(format \"%+d\" 1)", "invalid format operation"));
	CHECK(eval_error_contains(
	    "(format \"% d\" 1)", "invalid format operation"));
	CHECK(eval_error_contains(
	    "(format \"%#x\" 255)", "invalid format operation"));
	CHECK(eval_error_contains(
	    "(format \"%1$s\" 1)", "invalid format operation"));
	CHECK(eval_error_contains("(format \"%c\" 0)", "out of range"));
	/* Recovery after any of those leaves the interpreter usable. */
	CHECK(eval_eq("(format \"%s\" 'ok)", "ok"));

	/* A float rendering is bounded by nothing but the precision the
	 * caller wrote, and the conversion buffer is 512 bytes: kg used to
	 * ignore snprintf's return and answer the first 511 characters of
	 * (format "%.600f" 1.0), where Emacs 31.0.90 answers all 602.  The
	 * rows below straddle the buffer -- just under, just over, and over
	 * with a width and with left alignment, since those take different
	 * paths through the padding. */
	CHECK(eval_eq("(length (format \"%.500f\" 1.0))", "502"));
	CHECK(eval_eq("(length (format \"%.509f\" 1.0))", "511"));
	CHECK(eval_eq("(length (format \"%.510f\" 1.0))", "512"));
	CHECK(eval_eq("(length (format \"%.600f\" 1.0))", "602"));
	CHECK(eval_eq("(length (format \"%.600e\" 1.0))", "606"));
	CHECK(eval_eq("(length (format \"%700.600f\" 1.0))", "700"));
	CHECK(eval_eq("(length (format \"%-620.600f|\" 1.0))", "621"));
	CHECK(eval_eq("(substring (format \"%700.600f\" 1.0) 97 100)", " 1."));
	CHECK(eval_eq(
	    "(substring (format \"%-620.600f|\" 1.0) 617 621)", "   |"));
	/* A long rendering is still exact at its head. */
	CHECK(eval_eq("(substring (format \"%.600f\" 1.0) 0 6)", "1.0000"));

	/* message is format plus the status line. */
	CHECK(eval_ok("(message \"%s of %d\" \"one\" 3)"));
	CHECK(strcmp(test_status_message, "one of 3") == 0);
	CHECK(eval_ok("(message \"50%%\")"));
	CHECK(strcmp(test_status_message, "50%") == 0);
	CHECK(eval_error_contains("(message \"50%\")", "middle of format"));
	CHECK(eval_ok("(message \"a\" \"b\")"));
	CHECK(strcmp(test_status_message, "a") == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_char_string_round_trip(void)
{
	setup_utf8_buffer();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(char-to-string 97)", "a"));
	CHECK(eval_eq("(char-to-string 10)", "\n"));
	CHECK(eval_eq("(char-to-string 233)", "\xc3\xa9"));
	CHECK(eval_eq("(char-to-string 28450)", "\xe6\xbc\xa2"));
	CHECK(eval_eq("(char-to-string 128169)", "\xf0\x9f\x92\xa9"));
	CHECK(eval_error_contains("(char-to-string 0)", "out of range"));
	CHECK(eval_error_contains("(char-to-string 1114112)", "out of range"));
	CHECK(eval_error_contains("(char-to-string 55296)", "surrogate"));

	CHECK(eval_eq("(string-to-char \"abc\")", "97"));
	CHECK(eval_eq("(string-to-char \"\xc3\xa9x\")", "233"));
	CHECK(eval_eq("(string-to-char \"\")", "nil"));

	CHECK(eval_eq("(string-to-char (char-to-string 28450))", "28450"));
	CHECK(eval_eq("(string-length (char-to-string 28450))", "1"));
	/* char-after returns a number; char-to-string turns it back into text.
	 */
	CHECK(eval_eq("(char-to-string (char-after 6))", "\xc3\xa9"));
	CHECK(eval_eq("(char-to-string (char-after 11))", "\xe6\xbc\xa2"));
	CHECK(eval_eq("(string-to-char (buffer-substring 11 13))", "28450"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* "one  two" / "" / "héllo wörld" / "漢字" in 1-based codepoint positions:
 * o=1 n=2 e=3 ' '=4 ' '=5 t=6 w=7 o=8 \n=9, the empty row is 10, then
 * h=11 é=12 l=13 l=14 o=15 ' '=16 w=17 ö=18 r=19 l=20 d=21 \n=22, and
 * 漢=23 字=24 with point-max at 25. */
static void setup_thing_buffer(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "one  two", 8);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, "h\xc3\xa9llo w\xc3\xb6rld", 13);
	editor_insert_row(bcur(), 3, "\xe6\xbc\xa2\xe5\xad\x97", 6);
}

static void test_thing_at_point(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* An empty buffer has no thing at point, but a bad THING still
	 * raises rather than passing for "nothing here". */
	CHECK(eval_eq("(bounds-of-thing-at-point 'word)", "nil"));
	CHECK(eval_error_contains(
	    "(bounds-of-thing-at-point 'sexp)", "unsupported thing"));
	CHECK(eval_error_contains("(bounds-of-thing-at-point 'sexp)", "sexp"));
	CHECK(eval_error_contains(
	    "(bounds-of-thing-at-point \"word\")", "needs a symbol"));
	CHECK(eval_error_contains(
	    "(bounds-of-thing-at-point)", "too few arguments"));
	kg_lisp_shutdown();
	teardown_editor();

	setup_thing_buffer();
	CHECK(kg_lisp_init() == 0);

	/* Start, middle and end of a word all name the same word. */
	CHECK(eval_eq("(progn (goto-char 1) (bounds-of-thing-at-point 'word))",
	    "(1 . 4)"));
	CHECK(eval_eq("(progn (goto-char 2) (bounds-of-thing-at-point 'word))",
	    "(1 . 4)"));
	CHECK(eval_eq("(progn (goto-char 4) (bounds-of-thing-at-point 'word))",
	    "(1 . 4)"));
	CHECK(eval_eq("(progn (goto-char 6) (bounds-of-thing-at-point 'word))",
	    "(6 . 9)"));
	CHECK(eval_eq("(progn (goto-char 9) (bounds-of-thing-at-point 'word))",
	    "(6 . 9)"));
	/* Between two words, and on an empty line: no word. */
	CHECK(eval_eq(
	    "(progn (goto-char 5) (bounds-of-thing-at-point 'word))", "nil"));
	CHECK(eval_eq(
	    "(progn (goto-char 10) (bounds-of-thing-at-point 'word))", "nil"));

	/* Codepoints at or above U+0080 are word constituents here, unlike
	 * kg's ASCII-only interactive word motion. */
	CHECK(eval_eq("(progn (goto-char 11) (bounds-of-thing-at-point 'word))",
	    "(11 . 16)"));
	CHECK(eval_eq("(progn (goto-char 12) (bounds-of-thing-at-point 'word))",
	    "(11 . 16)"));
	CHECK(eval_eq("(progn (goto-char 18) (bounds-of-thing-at-point 'word))",
	    "(17 . 22)"));
	CHECK(eval_eq("(progn (goto-char 23) (bounds-of-thing-at-point 'word))",
	    "(23 . 25)"));
	CHECK(eval_eq("(progn (goto-char 24) (bounds-of-thing-at-point 'word))",
	    "(23 . 25)"));

	/* 'line takes in its line break, as in Emacs, so END is the start of
	 * the next row.  emacs -Q --batch on the same text returns (1 . 10),
	 * (10 . 11), (11 . 23) and (23 . 25). */
	CHECK(eval_eq("(progn (goto-char 5) (bounds-of-thing-at-point 'line))",
	    "(1 . 10)"));
	CHECK(eval_eq("(progn (goto-char 10) (bounds-of-thing-at-point 'line))",
	    "(10 . 11)"));
	CHECK(eval_eq("(progn (goto-char 15) (bounds-of-thing-at-point 'line))",
	    "(11 . 23)"));
	/* The last row has no next row, so its bounds stop at point-max. */
	CHECK(eval_eq("(progn (goto-char 23) (bounds-of-thing-at-point 'line))",
	    "(23 . 25)"));
	CHECK(eval_eq("(progn (goto-char 25) (bounds-of-thing-at-point 'line))",
	    "(23 . 25)"));

	/* car/cdr work on the result, as with any Emacs bounds pair. */
	CHECK(eval_eq(
	    "(car (progn (goto-char 12) (bounds-of-thing-at-point 'word)))",
	    "11"));
	CHECK(eval_eq(
	    "(cdr (progn (goto-char 12) (bounds-of-thing-at-point 'word)))",
	    "16"));

	/* The prelude's thing-at-point is the text of those bounds. */
	CHECK(eval_eq(
	    "(progn (goto-char 12) (thing-at-point 'word))", "h\xc3\xa9llo"));
	CHECK(eval_eq("(progn (goto-char 23) (thing-at-point 'word))",
	    "\xe6\xbc\xa2\xe5\xad\x97"));
	CHECK(eval_eq("(progn (goto-char 5) (thing-at-point 'word))", "nil"));
	/* The line break is part of the text, so it is part of the string. */
	CHECK(eval_eq(
	    "(progn (goto-char 5) (thing-at-point 'line))", "one  two\n"));
	CHECK(eval_eq("(progn (goto-char 10) (thing-at-point 'line))", "\n"));
	CHECK(eval_eq("(progn (goto-char 23) (thing-at-point 'line))",
	    "\xe6\xbc\xa2\xe5\xad\x97"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_prelude_forms(void)
{
	CHECK(kg_lisp_init() == 0);

	/* cond, including a t fallthrough and an exhausted cond. */
	CHECK(eval_eq("(cond (t 1))", "1"));
	CHECK(eval_eq("(cond (nil 1) (nil 2) (t 3))", "3"));
	CHECK(
	    eval_eq("(cond ((is 1 2) \"a\") ((is 1 1) \"b\") (t \"c\"))", "b"));
	CHECK(eval_eq("(cond (nil 1))", "nil"));
	CHECK(eval_eq("(cond)", "nil"));
	/* An exhausted cond is really nil, not a nil-shaped truthy object. */
	CHECK(eval_eq("(if (cond (nil 1)) \"yes\" \"no\")", "no"));
	CHECK(eval_eq("(not (cond (nil 1)))", "t"));
	/* Clause bodies are implicit progns and only the taken one runs. */
	CHECK(eval_eq("(progn (setq seen 0) (cond (t (setq seen 1) 2) (t (setq "
		      "seen 9))) seen)",
	    "1"));

	CHECK(eval_eq("(when t 1 2)", "2"));
	CHECK(eval_eq("(when nil 1)", "nil"));
	CHECK(eval_eq("(progn (setq n 0) (when t (setq n 5)) n)", "5"));
	CHECK(eval_eq("(progn (setq n 0) (when nil (setq n 5)) n)", "0"));
	CHECK(eval_eq("(unless nil 1 2)", "2"));
	CHECK(eval_eq("(unless t 1)", "nil"));
	CHECK(eval_eq("(progn (setq n 0) (unless nil (setq n 7)) n)", "7"));
	CHECK(eval_eq("(progn (setq n 0) (unless t (setq n 7)) n)", "0"));

	/* dolist accumulates and leaves the loop variable local. */
	CHECK(eval_eq(
	    "(progn (setq acc \"\") (dolist (x (list \"a\" \"b\" \"c\"))"
	    " (setq acc (concat acc x))) acc)",
	    "abc"));
	CHECK(eval_eq(
	    "(progn (setq n 0) (dolist (x (list 1 2 3)) (setq n (+ n x))) n)",
	    "6"));
	CHECK(eval_eq("(progn (setq n 0) (dolist (x nil) (setq n 1)) n)", "0"));
	/* Nested loops do not capture each other's variable. */
	CHECK(eval_eq("(progn (setq acc \"\") (dolist (a (list \"1\" \"2\"))"
		      " (dolist (b (list \"x\" \"y\"))"
		      "  (setq acc (concat acc a b)))) acc)",
	    "1x1y2x2y"));

	CHECK(eval_eq("(string-empty-p \"\")", "t"));
	CHECK(eval_eq("(string-empty-p \"a\")", "nil"));
	CHECK(eval_eq("(string-empty-p (substring \"abc\" 1 1))", "t"));

	kg_lisp_shutdown();
}

/* Fe's `if` used to be an elif chain, so a four-armed `if` silently ran
 * the third form as a condition and never reached the fourth. */
static void test_elisp_if(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(if t 1 2)", "1"));
	CHECK(eval_eq("(if nil 1 2)", "2"));
	CHECK(eval_eq("(if nil 1)", "nil"));
	CHECK(eval_eq("(if t 1)", "1"));
	/* The ELSE forms are an implicit progn: every one of them runs and the
	 * last one is the value. */
	CHECK(eval_eq(
	    "(progn (setq probe 0) (if nil 1 (setq probe 2) (setq probe 3))"
	    " probe)",
	    "3"));
	CHECK(eval_eq("(if nil 1 2 3)", "3"));
	/* A false `if` with no ELSE is really nil, not a nil-shaped object. */
	CHECK(eval_eq("(not (if nil 1))", "t"));
	CHECK(eval_eq("(is (if nil 1) nil)", "t"));
	/* Only the taken branch runs. */
	CHECK(eval_eq(
	    "(progn (setq probe 0) (if t 1 (setq probe 9)) probe)", "0"));
	CHECK(eval_eq(
	    "(progn (setq probe 0) (if nil (setq probe 9) 1) probe)", "0"));
	/* A `let` in the ELSE forms binds, because they are a do-list. */
	CHECK(eval_eq("(if nil 1 (internal--let q 5) q)", "5"));

	kg_lisp_shutdown();
}

static void test_list_library(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(reverse (list 1 2 3))", "(3 2 1)"));
	CHECK(eval_eq("(reverse nil)", "nil"));
	CHECK(eval_eq("(length (list 1 2 3))", "3"));
	CHECK(eval_eq("(length nil)", "0"));
	/* length is polymorphic over strings, as in Emacs. */
	CHECK(eval_eq("(length \"h\xc3\xa9llo\")", "5"));
	CHECK(eval_eq("(nth 0 (list 'a 'b))", "a"));
	CHECK(eval_eq("(nth 1 (list 'a 'b))", "b"));
	CHECK(eval_eq("(nth 9 (list 'a))", "nil"));
	CHECK(eval_eq("(nthcdr 2 (list 1 2 3 4))", "(3 4)"));
	CHECK(eval_eq("(nthcdr 9 (list 1))", "nil"));
	CHECK(eval_eq("(last (list 1 2 3))", "(3)"));
	CHECK(eval_eq("(last nil)", "nil"));
	CHECK(eval_eq("(append (list 1 2) (list 3) (list 4))", "(1 2 3 4)"));
	CHECK(eval_eq("(append)", "nil"));
	CHECK(eval_eq("(append nil (list 1))", "(1)"));
	CHECK(eval_eq("(append (list 1) nil)", "(1)"));
	CHECK(eval_eq("(mapcar (lambda (x) (* x 2)) (list 1 2 3))", "(2 4 6)"));
	CHECK(eval_eq("(mapcar (lambda (x) x) nil)", "nil"));
	CHECK(eval_eq("(member 3 (list 1 2 3 4))", "(3 4)"));
	CHECK(eval_eq("(member 9 (list 1))", "nil"));
	CHECK(eval_eq("(memq 'c (list 'a 'b 'c))", "(c)"));
	CHECK(eval_eq("(assoc 'b (list (cons 'a 1) (cons 'b 2)))", "(b . 2)"));
	CHECK(eval_eq("(assoc 'z (list (cons 'a 1)))", "nil"));
	CHECK(eval_eq("(assoc 'a nil)", "nil"));
	/* equal is structural on lists, where Fe's `is` compares identity. */
	CHECK(eval_eq("(equal (list 1 (list 2)) (list 1 (list 2)))", "t"));
	CHECK(eval_eq("(equal (list 1) (list 1 2))", "nil"));
	CHECK(eval_eq("(equal (list 1) 1)", "nil"));
	CHECK(eval_eq("(equal \"ab\" \"ab\")", "t"));
	CHECK(eval_eq("(equal nil nil)", "t"));
	/* 05E replaced the fe-native `is` tail with an eql leaf rule, so a
	 * number leaf agrees only when it is the same type *and* the same
	 * value: (equal 1 1.0) is nil, where `is` said t.  Emacs 31.0.90
	 * answers the same, and test/lisp-compat's equal-number-leaves pins
	 * it against a snapshot of that. */
	CHECK(eval_eq("(equal 1 1.0)", "nil"));
	CHECK(eval_eq("(equal 1.0 1.0)", "t"));
	CHECK(eval_eq("(equal 1 1)", "t"));
	CHECK(eval_eq("(equal (list 1) (list 1.0))", "nil"));
	CHECK(eval_eq("(eq 'a 'a)", "t"));
	CHECK(eval_eq("(null nil)", "t"));
	CHECK(eval_eq("(1+ 1)", "2"));
	CHECK(eval_eq("(1- 1)", "0"));
	CHECK(eval_eq("(cadr (list 1 2 3))", "2"));
	CHECK(eval_eq("(cddr (list 1 2 3))", "(3)"));
	CHECK(eval_eq("(caar (list (list 1)))", "1"));

	/* The list functions are iterative: a long list must not blow the GC
	 * stack the way a recursive implementation would. */
	CHECK(eval_eq("(progn (setq big nil) (setq i 0)"
		      " (while (< i 500) (setq big (cons i big))"
		      "  (setq i (+ i 1)))"
		      " (length (append (reverse big)"
		      "  (mapcar (lambda (x) x) big))))",
	    "1000"));

	kg_lisp_shutdown();
}

static void test_type_predicates(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(type-of \"a\")", "string"));
	CHECK(eval_eq("(type-of 'a)", "symbol"));
	CHECK(eval_eq("(type-of 1)", "integer"));
	CHECK(eval_eq("(type-of 1.0)", "double"));
	CHECK(eval_eq("(type-of (cons 1 2))", "pair"));
	CHECK(eval_eq("(type-of nil)", "nil"));
	/* The callables live in function cells now, so the type probe reads
	 * the cell through a designator, never a bare value read. */
	CHECK(eval_eq("(type-of (symbol-function 'car))", "primitive"));
	CHECK(eval_eq("(type-of (symbol-function 'insert))", "native-fn"));
	CHECK(eval_eq("(type-of (lambda (x) x))", "lambda"));
	CHECK(eval_eq("(type-of (symbol-function 'cond))", "macro"));

	CHECK(eval_eq("(stringp \"a\")", "t"));
	CHECK(eval_eq("(stringp 'a)", "nil"));
	CHECK(eval_eq("(numberp 1)", "t"));
	CHECK(eval_eq("(numberp 1.5)", "t"));
	CHECK(eval_eq("(numberp \"1\")", "nil"));
	CHECK(eval_eq("(consp (cons 1 2))", "t"));
	CHECK(eval_eq("(consp nil)", "nil"));
	/* nil and t are symbols in Emacs, and here too. */
	CHECK(eval_eq("(symbolp 'a)", "t"));
	CHECK(eval_eq("(symbolp nil)", "t"));
	CHECK(eval_eq("(symbolp \"a\")", "nil"));
	CHECK(eval_eq("(functionp (lambda (x) x))", "t"));
	/* A symbol is a function designator: it resolves through its function
	 * cell, so (functionp 'car) is t, a macro is not a function, and a
	 * name with an empty cell is not one either. */
	CHECK(eval_eq("(functionp 'car)", "t"));
	CHECK(eval_eq("(functionp 'insert)", "t"));
	/* A native registers in the function namespace only: the value cell is
	 * never touched, so (boundp 'insert) is nil while (fboundp 'insert) is
	 * t -- the Lisp-2 split observable on a real kg native. */
	CHECK(eval_eq("(boundp 'insert)", "nil"));
	CHECK(eval_eq("(fboundp 'insert)", "t"));
	CHECK(eval_eq("(functionp 'cond)", "nil"));
	CHECK(eval_eq("(functionp 'no-such-function)", "nil"));
	CHECK(eval_eq("(functionp 1)", "nil"));
	/* Emacs' answer for a special form is nil -- `if' is not a function,
	 * however callable its name looks -- and the same for a macro
	 * whichever namespace it came from.  kg asks Fe's FeIsFunction,
	 * which is the classification funcall and apply reject an
	 * `invalid-function' operand by, so the predicate and the
	 * interpreter agree; the hand-rolled type check this replaced said t
	 * for every primitive, `if' included. */
	CHECK(eval_eq("(functionp 'if)", "nil"));
	CHECK(eval_eq("(functionp 'quote)", "nil"));
	CHECK(eval_eq("(functionp 'while)", "nil"));
	CHECK(eval_eq("(functionp 'when)", "nil"));
	CHECK(eval_eq("(functionp 'let)", "nil"));
	/* A cyclic alias chain is answered, not raised at: functionp
	 * resolves the designator with fe's host-facing FeGetFunction, which
	 * yields nil for a cycle, and nil is not a function.  Emacs answers
	 * nil here too -- by a different route, since its own fset refuses
	 * to build the cycle in the first place. */
	CHECK(eval_ok("(fset 'cyc 'cyc)"));
	CHECK(eval_eq("(functionp 'cyc)", "nil"));
	/* Contained, not merely quiet: the interpreter is still usable, and
	 * calling the name is still an error rather than a crash. */
	CHECK(eval_eq("(+ 1 2)", "3"));
	CHECK(eval_error_contains(
	    "(funcall 'cyc)", "cyclic-function-indirection"));
	/* fboundp reads the raw cell and follows nothing, so it answers for
	 * the same name without raising -- the "never errors" doc/lisp-api.md
	 * claims for it. */
	CHECK(eval_eq("(fboundp 'cyc)", "t"));
	CHECK(eval_eq("(listp nil)", "t"));
	CHECK(eval_eq("(listp (cons 1 2))", "t"));
	CHECK(eval_eq("(listp 1)", "nil"));
	CHECK(eval_error_contains("(stringp)", "too few arguments"));
	CHECK(eval_error_contains("(type-of 1 2)", "too many arguments"));

	kg_lisp_shutdown();
}

/* The fe-side numeric corrections kg's Phase 5 review put on the pin,
 * asserted through kg's own Lisp because kg is where some of them are
 * reachable at all: kg links core fe *without* Fex, so the `floor`,
 * `ceiling`, `round` and `truncate` running here are fe's own, where the
 * standalone fe interpreter's are Fex's shadowing definitions.  A NaN
 * operand used to reach an out-of-range double-to-int64 conversion on
 * exactly this path -- undefined behaviour observable only from a host
 * like kg -- so these cases are the evidence for that fix, not a
 * duplicate of fe's suite.  Every comparator expectation below was run
 * against Emacs 31.0.90, which answers the same. */
static void test_numeric_core_error_rules(void)
{
	CHECK(kg_lisp_init() == 0);

	/* Rounding: a NaN operand, and a zero divisor in the two-argument
	 * form, are arith-error rather than a UB conversion or a silent
	 * value.  The NaN is spelled (/ 0.0 0) because (/ 0 0) is integer
	 * division and raises before the rounding native ever runs. */
	CHECK(eval_error_contains("(floor (/ 0.0 0))", "arith-error"));
	CHECK(eval_error_contains("(ceiling (/ 0.0 0))", "arith-error"));
	CHECK(eval_error_contains("(round (/ 0.0 0))", "arith-error"));
	CHECK(eval_error_contains("(truncate (/ 0.0 0))", "arith-error"));
	CHECK(eval_error_contains("(floor 0 0)", "arith-error"));
	CHECK(eval_error_contains("(round 0 0)", "arith-error"));
	/* Contained, not merely quiet: the ordinary answers are unchanged
	 * and the interpreter is still usable afterwards. */
	CHECK(eval_eq("(floor 3.7)", "3"));
	CHECK(eval_eq("(floor 7 2)", "3"));
	CHECK(eval_eq("(ceiling 3.2)", "4"));
	CHECK(eval_eq("(round 3.5)", "4"));

	/* One operand is t with no type check at all -- Emacs' pair loop has
	 * no pair to run, so it never asks what the operand is. */
	CHECK(eval_eq("(= \"a\")", "t"));
	CHECK(eval_eq("(< t)", "t"));
	CHECK(eval_eq("(= 3)", "t"));
	/* A chain stops at the first false pair, so an operand past a
	 * settled answer is never type-checked; one that is still reachable
	 * is.  Both operand forms were evaluated before either arm ran, so
	 * short-circuiting erases no side effect. */
	CHECK(eval_eq("(< 2 1 \"a\")", "nil"));
	CHECK(eval_error_contains("(< 1 2 \"a\")", "wrong-type-argument"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* `is` is fe's own broad comparator, not an Emacs form: it regained
	 * its epsilon tolerance across the two number tags, where `eql` --
	 * which is what `equal`'s leaves use -- stays type-honest. */
	CHECK(eval_eq("(is 1 1.0)", "t"));
	CHECK(eval_eq("(eql 1 1.0)", "nil"));

	/* The reader wants an explicit sign in a nonfinite exponent, so this
	 * is a symbol again rather than a number. */
	CHECK(eval_eq("(type-of '1eINF)", "symbol"));
	CHECK(eval_eq("(type-of '1.0e+INF)", "double"));

	kg_lisp_shutdown();
}

static void test_binding_forms(void)
{
	CHECK(kg_lisp_init() == 0);

	/* let is parallel: the value forms see the outer bindings. */
	CHECK(eval_eq("(progn (setq y 100) (let ((y 1) (z y)) z))", "100"));
	CHECK(eval_eq("(progn (setq y 100) (let* ((y 1) (z y)) z))", "1"));
	CHECK(eval_eq("(let ((a 1) (b 2)) (+ a b))", "3"));
	CHECK(eval_eq(
	    "(let* ((p 1) (q (+ p 1)) (r (+ q 1))) (list p q r))", "(1 2 3)"));
	/* Bare symbols and value-less bindings are nil. */
	CHECK(eval_eq("(let ((a 1) b (c)) (list a b c))", "(1 nil nil)"));
	CHECK(eval_eq("(let () 42)", "42"));
	CHECK(eval_eq("(let* () 42)", "42"));
	/* Nested lets shadow. */
	CHECK(eval_eq("(let ((a 1)) (let ((a 2) (b a)) (list a b)))", "(2 1)"));
	/* A while loop with setq inside a let body. */
	CHECK(eval_eq("(let ((acc 0) (k 0))"
		      " (while (< k 5) (setq acc (+ acc k)) (setq k (+ k 1)))"
		      " acc)",
	    "10"));
	/* setq inside a let reaches an outer variable. */
	CHECK(eval_eq(
	    "(progn (setq outer 0) (let ((q 1)) (setq outer 7)) outer)", "7"));
	/* Closures capture the let bindings.  The closure is a value, so the
	 * call goes through funcall: call position resolves function cells,
	 * and a bare (mk 5) would now be void-function. */
	CHECK(eval_eq("(progn (setq mk (let ((n 10)) (lambda (x) (+ x n))))"
		      " (funcall mk 5))",
	    "15"));

	/* setq takes any number of pairs and returns the last value. */
	CHECK(eval_eq("(progn (setq a 1 b 2 c 3) (list a b c))", "(1 2 3)"));
	CHECK(eval_eq("(setq a 5)", "5"));
	CHECK(eval_eq("(setq a 1 b 2)", "2"));
	CHECK(eval_eq("(setq)", "nil"));

	CHECK(eval_eq("(prog1 1 2 3)", "1"));
	CHECK(eval_eq(
	    "(progn (setq l (list 1 2 3)) (list (pop l) l))", "(1 (2 3))"));
	CHECK(eval_eq("(progn (setq l (list 2)) (push 1 l) l)", "(1 2)"));
	CHECK(eval_eq(
	    "(progn (setq n 0) (dotimes (i 4) (setq n (+ n i))) n)", "6"));
	CHECK(eval_eq("(dotimes (i 2) i)", "nil"));
	CHECK(eval_eq("(dotimes (i 2 'done) i)", "done"));
	CHECK(eval_eq("(dolist (x (list 1 2) 'done) x)", "done"));

	/* The phase's headline assertion: the value and function namespaces
	 * coexist under one name, as in Emacs -- defun writes the function
	 * cell and leaves the value cell alone. */
	CHECK(
	    eval_eq("(progn (setq f 7) (defun f () 9) (list f (f)))", "(7 9)"));

	kg_lisp_shutdown();
}

/* Phase 11's semantics grid, through kg's own entry points.
 *
 * test/lisp-compat/cases/phase11-dynamic-*.json compare the same probes
 * against the pinned Emacs 31.0.90 and are the authority on what the
 * answers should be; this runs them in-process, where a failure names a
 * line instead of a case, and adds the shapes the compat corpus cannot
 * reach -- the interaction with kg's own binding forms, and the
 * degenerate `let' shapes that have nothing to do with Emacs
 * disagreement and everything to do with not regressing kg.
 *
 * The grid probe each assertion is is named in the comment above it.
 * Six of the twenty-one probes ALREADY agreed before Phase 11 and are
 * guards rather than targets; they are marked as such, because an
 * implementation that binds specials dynamically at every
 * parameter-binding site passes every target here and fails A4. */
static void test_phase11_dynamic_binding(void)
{
	CHECK(kg_lisp_init() == 0);

	/* A9a: the predicate exists, and answers the *special* flag.  The
	 * three constants answer t, which is Emacs' answer for a name
	 * nothing can ever bind. */
	CHECK(eval_eq("(special-variable-p t)", "t"));
	CHECK(eval_eq("(special-variable-p nil)", "t"));
	CHECK(eval_eq("(special-variable-p :kw)", "t"));
	CHECK(eval_eq("(special-variable-p 'p11-never-declared)", "nil"));
	CHECK(eval_error_contains(
	    "(special-variable-p 3)", "expected symbol, got integer"));

	/* A7d / A8a: a two-argument defvar and a defconst mark full;
	 * A7a: a one-argument defvar marks let-dynamic only, so the
	 * predicate still answers nil for it. */
	CHECK(eval_eq("(progn (defvar p11-two 1) (special-variable-p"
		      " 'p11-two))",
	    "t"));
	CHECK(eval_eq("(progn (defconst p11-const 1) (special-variable-p"
		      " 'p11-const))",
	    "t"));
	CHECK(eval_eq("(progn (defvar p11-one) (special-variable-p"
		      " 'p11-one))",
	    "nil"));
	/* A10b (guard): a one-argument defvar binds nothing. */
	CHECK(eval_eq("(boundp 'p11-one)", "nil"));
	/* A7b: and yet `let' over it is dynamic.  kg's marking is global
	 * where Emacs scopes this to the file; the compat row
	 * phase11-one-arg-defvar-file-scope pins both answers. */
	CHECK(eval_eq("(progn (defun p11-one-r () p11-one)"
		      " (let ((p11-one 5)) (p11-one-r)))",
	    "5"));

	/* A1 / A11: the temporary-setting idiom, the headline. */
	CHECK(eval_eq("(progn (defvar p11-hkv nil) (defun p11-callee ()"
		      " p11-hkv) (defun p11-caller () (let ((p11-hkv t))"
		      " (p11-callee))) (p11-caller))",
	    "t"));
	/* A3a: nested extents, global outside and bound value inside. */
	CHECK(eval_eq("(progn (defvar p11-nv 1) (defun p11-nvf () p11-nv)"
		      " (list (p11-nvf) (let ((p11-nv 2)) (p11-nvf))"
		      " (p11-nvf)))",
	    "(1 2 1)"));
	/* A2a: setq inside the binding writes the binding, not the value
	 * the binding hides. */
	CHECK(eval_eq("(progn (defvar p11-sv 1) (defun p11-svf () p11-sv)"
		      " (list (let ((p11-sv 2)) (setq p11-sv 3) (p11-svf))"
		      " p11-sv))",
	    "(3 1)"));
	/* A2b (guard): setq outside any let still writes the global. */
	CHECK(eval_eq("(progn (setq p11-sv 4) p11-sv)", "4"));
	/* A3b: let* binds sequentially, and the path is special-aware. */
	CHECK(eval_eq("(progn (defvar p11-ls 1) (defun p11-lsf () p11-ls)"
		      " (let* ((p11-ls 2) (o (p11-lsf))) (list p11-ls o)))",
	    "(2 2)"));
	/* A5: a closure reads the value in force when it is CALLED. */
	CHECK(eval_eq("(progn (defvar p11-cv 1)"
		      " (let ((f (lambda () p11-cv)))"
		      " (let ((p11-cv 2)) (funcall f))))",
	    "2"));
	/* A8b: defconst declares, it does not enforce -- still
	 * let-rebindable, and the rebinding is dynamic. */
	CHECK(eval_eq("(progn (defun p11-cf () p11-const)"
		      " (list (let ((p11-const 2)) (p11-cf)) (p11-cf)))",
	    "(2 1)"));
	/* A10a: binding an unbound special binds it, and the restore puts
	 * back the ABSENCE of a value rather than nil. */
	CHECK(eval_eq("(progn (defvar p11-uv 1) (defun p11-uvf ()"
		      " (if (boundp 'p11-uv) (list 'bound p11-uv) 'unbound))"
		      " (makunbound 'p11-uv)"
		      " (list (let ((p11-uv 3)) (p11-uvf)) (p11-uvf)))",
	    "((bound 3) unbound)"));

	/* The restore runs on every completion kind.  A6a is the throw,
	 * A6b (guard -- kg and Emacs agreed before Phase 11 too) the
	 * error; quit has no in-process probe and is covered by the
	 * exhaustion/quit cases elsewhere in this file. */
	CHECK(eval_eq("(progn (defvar p11-tv 1) (defun p11-tvf () p11-tv)"
		      " (list (catch 'tag (let ((p11-tv 2))"
		      " (throw 'tag (p11-tvf)))) (p11-tvf)))",
	    "(2 1)"));
	CHECK(eval_eq("(progn (defvar p11-ev 1) (defun p11-evf () p11-ev)"
		      " (list (condition-case nil (let ((p11-ev 2))"
		      " (error \"boom\")) (error (p11-evf))) (p11-evf)))",
	    "(1 1)"));
	/* And on the budget wall, which no compat case can reach and
	 * `ignore-errors' cannot contain -- Budget is uncatchable by
	 * design, so the eval below is expected to FAIL, and the claim is
	 * about what the arena looks like afterwards: the binding must
	 * not have survived the wall. */
	CHECK(eval_ok("(defvar p11-bv 1)"));
	CHECK(!eval_ok("(let ((p11-bv 2)) (while t (setq p11-bv 2)))"));
	CHECK(eval_eq("p11-bv", "1"));

	/* A4 (guard, and the probe that falsified the phase's first
	 * draft): a defun PARAMETER named after a special binds
	 * LEXICALLY, in Emacs 31 under lexical-binding: t and here. */
	CHECK(eval_eq("(progn (defvar p11-pv 1) (defun p11-reader ()"
		      " p11-pv) (defun p11-taker (p11-pv)"
		      " (list p11-pv (p11-reader))) (p11-taker 9))",
	    "(9 1)"));
	/* The same for a lambda's parameter. */
	CHECK(eval_eq("(funcall (lambda (p11-pv) (list p11-pv"
		      " (p11-reader))) 8)",
	    "(8 1)"));
	/* A13 (guard): a name no defvar ever named stays lexical. */
	CHECK(eval_eq("(progn (setq p11-plain 1) (defun p11-plainf ()"
		      " p11-plain) (list (let ((p11-plain 2)) (p11-plainf))"
		      " (p11-plainf)))",
	    "(1 1)"));
	/* A12 (guard): defvar is first-wins, and marking is idempotent
	 * and one-way -- a second, one-argument defvar over a fully
	 * marked name neither unmarks it nor reassigns it. */
	CHECK(eval_eq("(progn (defvar p11-two 99) (list p11-two"
		      " (special-variable-p 'p11-two)))",
	    "(1 t)"));
	CHECK(eval_eq("(progn (defvar p11-two) (special-variable-p"
		      " 'p11-two))",
	    "t"));
	/* Marking upgrades the other way: let-dynamic-only, then full. */
	CHECK(eval_eq("(progn (defvar p11-up) (defvar p11-up 1)"
		      " (special-variable-p 'p11-up))",
	    "t"));
	/* Constants cannot be marked at all. */
	CHECK(eval_error_contains("(defvar t 1)", "setting-constant"));

	/* Not grid probes: kg's own `let' shapes, which the switch onto
	 * fe's core bindings-list form had to preserve exactly.  The
	 * one-element binding (a) is why the prelude keeps a normalising
	 * macro instead of deleting it -- fe's core form raises
	 * wrong-type-argument for it, Emacs and kg answer nil. */
	CHECK(eval_eq("(let ((a)) a)", "nil"));
	CHECK(eval_eq("(let (a b) (list a b))", "(nil nil)"));
	CHECK(eval_eq("(let () 42)", "42"));
	CHECK(eval_eq("(let ((a 1)))", "nil"));
	/* kg's long-standing reading of a shape Emacs rejects; unchanged
	 * behaviour, recorded rather than fixed by this phase. */
	CHECK(eval_eq("(let ((a 1 2)) a)", "1"));
	/* The constant refusals survive the switch, with one message. */
	CHECK(eval_error_contains("(let ((t 1)) t)", "setting-constant"));
	CHECK(eval_error_contains("(let ((nil 1)) 1)", "setting-constant"));
	CHECK(eval_error_contains("(let ((:k 1)) 1)", "setting-constant"));
	CHECK(eval_error_contains("(let* ((t 1)) t)", "setting-constant"));
	/* Parallel evaluation of the initializers survives it too. */
	CHECK(eval_eq("(progn (setq p11-par 100)"
		      " (let ((p11-par 1) (z p11-par)) z))",
	    "100"));

	kg_lisp_shutdown();
}

static void test_definition_forms(void)
{
	CHECK(kg_lisp_init() == 0);
	CHECK(eval_eq("(prefix-numeric-value nil)", "1"));
	CHECK(eval_eq("(prefix-numeric-value '(16))", "16"));
	CHECK(eval_eq("(prefix-numeric-value '-)", "-1"));
	CHECK(eval_error_contains(
	    "(prefix-numeric-value '(1 2))", "wrong-type-argument"));

	/* defun returns the symbol, as Emacs does. */
	CHECK(eval_eq("(defun square (x) (* x x))", "square"));
	CHECK(eval_eq("(square 7)", "49"));
	/* Argument lists reach Fe as written: its binder reads &optional and
	 * &rest itself, so kg no longer lowers them to a dotted list. */
	CHECK(eval_ok("(defun bare args args)"));
	CHECK(eval_eq("(bare 1 2)", "(1 2)"));
	CHECK(eval_ok("(defun dotted (a . r) (list a r))"));
	CHECK(eval_eq("(dotted 1 2 3)", "(1 (2 3))"));
	CHECK(eval_ok("(defun none () 7)"));
	CHECK(eval_eq("(none)", "7"));
	CHECK(eval_ok("(defun both (a &optional b &rest r) (list a b r))"));
	CHECK(eval_eq("(both 1)", "(1 nil nil)"));
	CHECK(eval_eq("(both 1 2 3 4)", "(1 2 (3 4))"));

	CHECK(eval_ok("(defun opt (a &optional b) (list a b))"));
	CHECK(eval_eq("(opt 1)", "(1 nil)"));
	CHECK(eval_eq("(opt 1 2)", "(1 2)"));
	CHECK(eval_ok("(defun rst (a &rest r) (list a r))"));
	CHECK(eval_eq("(rst 1)", "(1 nil)"));
	CHECK(eval_eq("(rst 1 2 3)", "(1 (2 3))"));

	/* A docstring is inert when a body follows it and is the value when it
	 * is the whole body, exactly as in Emacs. */
	CHECK(eval_ok("(defun documented (x) \"Doc.\" (+ x 1))"));
	CHECK(eval_eq("(documented 1)", "2"));
	CHECK(eval_ok("(defun onlydoc (x) \"Just a doc.\")"));
	CHECK(eval_eq("(onlydoc 1)", "Just a doc."));

	CHECK(eval_eq(
	    "(defmacro twice (form) (list 'progn form form))", "twice"));
	CHECK(eval_eq("(progn (setq n 0) (twice (setq n (+ n 1))) n)", "2"));
	CHECK(eval_ok("(defmacro my-list (&rest body) (cons 'list body))"));
	CHECK(eval_eq("(my-list 1 2)", "(1 2)"));

	/* defvar initialises only an unbound variable; defconst always
	 * assigns.  A variable holding nil is bound, so defvar leaves it. */
	CHECK(eval_eq("(defvar dv 5)", "dv"));
	CHECK(eval_ok("(defvar dv 9)"));
	CHECK(eval_eq("dv", "5"));
	CHECK(eval_ok("(setq dvn nil)"));
	CHECK(eval_ok("(defvar dvn 9)"));
	CHECK(eval_eq("dvn", "nil"));
	CHECK(eval_eq("(defconst dc 5)", "dc"));
	CHECK(eval_ok("(defconst dc 9)"));
	CHECK(eval_eq("dc", "9"));

	/* A stray top-level (interactive) is inert and really nil. */
	CHECK(eval_eq("(interactive)", "nil"));
	CHECK(eval_eq("(not (interactive))", "t"));

	/* (interactive) in a defun registers the command under its symbol and
	 * is stripped from the body. */
	setup_editor();
	CHECK(eval_ok("(defun greet-lisp () (interactive) (message \"hi\"))"));
	CHECK(kg_lisp_command_name(0) != nullptr);
	CHECK(strcmp(kg_lisp_command_name(0), "greet-lisp") == 0);
	CHECK(kg_lisp_run_command("greet-lisp", 0) == 0);
	CHECK(strcmp(test_status_message, "hi") == 0);
	CHECK(eval_eq("(defun greet-lisp () 9)", "greet-lisp"));
	CHECK(!kg_lisp_command_exists("greet-lisp"));
	CHECK(eval_ok("(defun late-lisp () 7 (interactive))"));
	CHECK(!kg_lisp_command_exists("late-lisp"));
	CHECK(eval_ok("(defun doc-empty-lisp () \"Doc\" (interactive))"));
	CHECK(kg_lisp_command_exists("doc-empty-lisp"));
	CHECK(kg_lisp_run_command("doc-empty-lisp", 0) == 0);
	/* A docstring plus a declaration and no executable form is nil, not
	 * the docstring: the declaration is removed after the docstring and
	 * the empty body becomes (nil).  07D item 1, Emacs-confirmed. */
	CHECK(eval_eq("(doc-empty-lisp)", "nil"));
	/* A declaration with no docstring behaves the same way ... */
	CHECK(eval_ok("(defun decl-only-lisp () (interactive))"));
	CHECK(eval_eq("(decl-only-lisp)", "nil"));
	/* ... and a string *after* the declaration is an ordinary body form,
	 * so it is the value.  Only the string in the Emacs docstring
	 * position, with a form after it, is documentation. */
	CHECK(eval_ok("(defun decl-then-lisp () (interactive) \"tail\")"));
	CHECK(eval_eq("(decl-then-lisp)", "tail"));

	/* commandp answers the command registry's question: only a name is
	 * a command here, and only the declaration in the Emacs position
	 * makes it one.  Every value below is Emacs' own answer, recorded
	 * in test/lisp-compat/oracle/interactive-command-predicate.json. */
	CHECK(eval_eq("(commandp 'greet-lisp)", "nil"));
	CHECK(eval_eq("(commandp 'decl-only-lisp)", "t"));
	CHECK(eval_eq("(commandp 'late-lisp)", "nil"));
	CHECK(eval_eq("(commandp 'quiet-lisp)", "nil"));
	/* A built-in editor command is one too, ... */
	CHECK(eval_eq("(commandp 'version)", "t"));
	/* ... and anything that is not a name at all is nil, as in Emacs. */
	CHECK(eval_eq("(commandp 5)", "nil"));
	CHECK(eval_eq("(commandp nil)", "nil"));
	CHECK(eval_eq("(commandp (lambda () 1))", "nil"));
	/* A defun without (interactive) registers nothing. */
	CHECK(eval_ok("(defun quiet-lisp () (message \"no\"))"));
	CHECK(!kg_lisp_command_exists("quiet-lisp"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_strict_arity(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq(
	    "(condition-case e (funcall (lambda (x) x)) "
	    "(wrong-number-of-arguments (list (car e) (car (cdr (cdr e))))))",
	    "(wrong-number-of-arguments 0)"));
	CHECK(
	    eval_eq("(condition-case e "
		    "(progn (defun needs-two (a b) (list a b)) (needs-two 1)) "
		    "(wrong-number-of-arguments (car e)))",
		"wrong-number-of-arguments"));
	CHECK(eval_eq("(condition-case e "
		      "(progn (defmacro needs-form (x) x) (needs-form 1 2)) "
		      "(wrong-number-of-arguments (car e)))",
	    "wrong-number-of-arguments"));
	CHECK(eval_eq("(progn (defun optional-rest (a &optional b &rest r) "
		      "(list a b r)) (optional-rest 1))",
	    "(1 nil nil)"));
	CHECK(eval_eq("(progn (defun optional-rest-2 (a &optional b &rest r) "
		      "(list a b r)) (optional-rest-2 1 2 3 4))",
	    "(1 2 (3 4))"));
	CHECK(eval_eq("(condition-case e (message) "
		      "(wrong-number-of-arguments (car e)))",
	    "wrong-number-of-arguments"));
	CHECK(eval_error_contains("(message)", "too few arguments"));
	CHECK(eval_eq("(condition-case e (goto-char 1 2) "
		      "(wrong-number-of-arguments (car e)))",
	    "wrong-number-of-arguments"));
	CHECK(eval_error_contains("(goto-char 1 2)", "too many arguments"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The writer abbreviates a two-element (quote X) list back to 'X, as
 * Emacs' printer does (fe sub-plan 11C, adopted at Phase 11's pin; this
 * was the recorded writer-quote-abbreviation divergence until then).  The
 * discrimination is the existing `function`/#' block's, measured against
 * Emacs in the same slice: exactly one element after the head, and the
 * form proper.  (quote x y), (quote) and (quote . x) all keep printing as
 * the pairs they are -- the answers on the right below are the pinned
 * Emacs 31.0.90's own.
 *
 * The reader's half stays here on purpose: 'x reads into the two-element
 * list (quote x) in kg exactly as it does in Emacs, which is what made
 * this the printer's gap and not the reader's, and what lets the compat
 * case build its operand with `list` and say the same thing to both
 * dialects. */
static void test_writer_quote_abbreviation(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(format \"%S\" (list 'quote 'x))", "'x"));
	CHECK(eval_eq("(format \"%S\" ''x)", "'x"));
	/* Recursive, so a quote inside a list comes out abbreviated too. */
	CHECK(eval_eq("(format \"%S\" (list 'a ''b 'c))", "(a 'b c)"));
	CHECK(eval_eq("(format \"%S\" '''x)", "''x"));
	/* Not a two-element proper (quote X): printed as the pair it is. */
	CHECK(eval_eq("(format \"%S\" (list 'quote 'x 'y))", "(quote x y)"));
	CHECK(eval_eq("(format \"%S\" (list 'quote))", "(quote)"));
	CHECK(eval_eq("(format \"%S\" (cons 'quote 'x))", "(quote . x)"));
	/* The reader's half: 'x is that list, element for element. */
	CHECK(eval_eq("(car ''x)", "quote"));
	CHECK(eval_eq("(car (cdr ''x))", "x"));
	CHECK(eval_eq("(length ''x)", "2"));

	kg_lisp_shutdown();
}

static void test_quasiquote(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("'`(a ,b ,@c)",
	    "(quasiquote (a (unquote b) (unquote-splicing c)))"));
	CHECK(eval_eq("(progn (setq b 42) `(a ,b c))", "(a 42 c)"));
	CHECK(eval_eq("(progn (setq l (list 7 8)) `(a ,@l c))", "(a 7 8 c)"));
	CHECK(eval_eq("`(a b)", "(a b)"));
	CHECK(eval_eq(
	    "(progn (setq b 42) `(nested (deep ,b)))", "(nested (deep 42))"));
	CHECK(eval_eq("(let ((x 2)) `(outer `(inner ,x)))",
	    "(outer (quasiquote (inner (unquote x))))"));
	CHECK(eval_eq("(let ((x 2)) `(outer `(inner ,,x)))",
	    "(outer (quasiquote (inner (unquote 2))))"));
	CHECK(eval_eq("(progn (setq b 42) `,b)", "42"));
	/* An empty backquote is a real nil, not a nil-shaped object. */
	CHECK(eval_eq("`()", "nil"));
	CHECK(eval_eq("(not `())", "t"));
	/* Backticks and commas inside strings stay inert. */
	CHECK(eval_eq("\"a, b `c` ,@d\"", "a, b `c` ,@d"));
	/* #'x reads as (function x), the Lisp-2 quote of a function name:
	 * it evaluates to the symbol designator, resolved when the call
	 * happens. */
	CHECK(eval_eq("(mapcar #'car (list (list 1 2) (list 3 4)))", "(1 3)"));
	CHECK(eval_eq("(function car)", "car"));
	/* #' over a lambda *form* is the other half of the reader
	 * abbreviation: (function (lambda ...)) evaluates the form to the
	 * closure, so the funcall below has a real function to call and not
	 * a quoted list. */
	CHECK(eval_eq("(funcall #'(lambda (n) (+ n 1)) 41)", "42"));
	CHECK(eval_eq("(functionp #'(lambda (n) n))", "t"));

	/* Dotted quasiquote.  Every row is GNU Emacs 31.0.90's answer
	 * (/opt-3/emacs-31-lucid, `emacs -Q --batch`).  The first two are
	 * the ones the reader makes hard: `,x' reads as the two-element
	 * list (unquote x), so `(1 . ,x)' is the PROPER list (1 unquote x)
	 * and the expander has to recognise an unquote form in cdr position
	 * rather than wait for an improper tail.  kg answered
	 * (1 unquote x) and (a b unquote x) for those, and `(a . b)' --
	 * a plain improper tail, which used to work -- raised
	 * void-variable list. */
	CHECK(eval_eq("(let ((x 2)) `(1 . ,x))", "(1 . 2)"));
	CHECK(eval_eq("(let ((x 2)) `(a b . ,x))", "(a b . 2)"));
	CHECK(eval_eq("`(a . b)", "(a . b)"));
	/* A splice in front of a dotted tail, and an unquoted nil tail,
	 * which Emacs answers as the proper list (a). */
	CHECK(eval_eq("(let ((x (list 1 2))) `(,@x . 9))", "(1 2 . 9)"));
	CHECK(eval_eq("`(a . ,nil)", "(a)"));
	/* The rule is structural, not syntactic: a literal `unquote' in cdr
	 * position means the same thing as the `,' that reads to it.  This
	 * row is kg's spelling, not Emacs': Emacs' reader produces the
	 * symbol named "," where fe's produces `unquote' (a recorded
	 * divergence in doc/fe-upstream.md), so Emacs answers
	 * (a unquote x) here while kg answers (a . 2). */
	CHECK(eval_eq("(let ((x 2)) `(a unquote x))", "(a . 2)"));

	/* A macro written with backquote. */
	CHECK(eval_ok("(defmacro unless2 (test . body)"
		      " `(if ,test nil (progn ,@body)))"));
	CHECK(eval_eq("(progn (setq n 0) (unless2 nil (setq n 5)) n)", "5"));
	CHECK(eval_eq("(progn (setq n 0) (unless2 t (setq n 5)) n)", "0"));

	kg_lisp_shutdown();
}

/* The improved diagnostic for calling something that is not a function. */
static void test_void_function(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_error_contains(
	    "(no-such-function 1)", "void-function no-such-function"));
	CHECK(eval_error_contains("(1 2)", "non-callable"));
	/* The Lisp-2 message reality: a name with only a value binding is
	 * void-function in call position -- call position sees the function
	 * cell and nothing else. */
	CHECK(eval_error_contains("(progn (setq v 7) (v))", "void-function v"));
	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
}

/* A name that was never assigned is an error, not nil. */
static void test_void_variable(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(boundp 'no-such-variable)", "nil"));
	CHECK(eval_error_contains(
	    "no-such-variable", "void-variable no-such-variable"));
	CHECK(eval_error_contains(
	    "(+ 1 no-such-variable)", "void-variable no-such-variable"));

	/* Assignment creates the binding, and nil is a value like any other. */
	CHECK(eval_ok("(setq no-such-variable nil)"));
	CHECK(eval_eq("(boundp 'no-such-variable)", "t"));
	CHECK(eval_eq("no-such-variable", "nil"));
	CHECK(eval_eq("(makunbound 'no-such-variable)", "no-such-variable"));
	CHECK(eval_eq("(boundp 'no-such-variable)", "nil"));

	/* The mirror image: a name with only a function binding is
	 * void-variable in value position -- the namespaces are separate. */
	CHECK(eval_ok("(defalias 'fn-only (lambda () 1))"));
	CHECK(eval_error_contains("fn-only", "void-variable fn-only"));

	/* boundp sees lexical bindings too. */
	CHECK(eval_eq("((lambda (p) (boundp 'p)) 1)", "t"));
	CHECK(eval_eq("(boundp 'p)", "nil"));

	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
}

/* A structure that refers to itself used to make the writer loop forever,
 * with no C-g out of it: M-:, eval-buffer and C-j all render their result. */
static void test_cyclic_result(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq(
	    "(progn (setq c (cons 1 nil)) (setcdr c c) c)", "(1 . #<cycle>)"));
	CHECK(eval_eq("(progn (setq d (cons 1 (cons 2 nil)))"
		      " (setcdr (cdr d) d) d)",
	    "(1 2 1 . #<cycle>)"));
	/* Shared but acyclic structure is still printed in full. */
	CHECK(eval_eq("(progn (setq s '(1 2)) (list s s))", "((1 2) (1 2))"));
	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
}

/* Lisp nesting is bounded by Fe's frame stack, not by how many GC stack
 * slots or how much C stack a call happens to consume (sub-plan 03F split
 * the old single evaluation_depth counter into two: this is the
 * `max_frames`/`frame_capacity` bound, "Lisp nesting"; native re-entry has
 * its own bound and error, exercised in fe/test_api.c, not here).
 * `(deep N)` now raises "evaluation frame limit exceeded" once N's frames
 * exceed the arena's frame capacity, instead of the accidental
 * "GC stack overflow" the pre-frame-machine evaluator could hit.
 *
 * Measured on this build via kg_lisp_arena_stats(): frame_capacity is
 * 1095, and `(deep 200)` alone reaches peak_frame_depth 604 -- about 3.02
 * frames per recursion level for this chain's shape (`if`, `+`, and the
 * recursive call each open a frame). `(deep 1000000)` therefore asks for
 * roughly 3 million frames against a 1095-frame arena, more than 2700x
 * over capacity -- demonstrably above it without depending on the private
 * Fe frame-size struct or reverse-engineering the arena layout, only on
 * the public frame_capacity/peak_frame_depth counters this file already
 * asserts through. (frame_capacity itself is arena-derived and not
 * asserted to a specific number here, since a KG_LISP_ARENA_SIZE override
 * or a Fe-side frame-size change would move it without changing this
 * test's property.)
 *
 * The probe below also confirms 03C's requirement empirically: after the
 * error, allocation_failures is still 0 and peak_frame_depth sits exactly
 * at frame_capacity -- the frame bound fired before the arena was ever
 * asked for an object it didn't have, so deep recursion raises a clean
 * evaluator error rather than an allocation failure.
 *
 * The final eval_ok calls are the property that actually matters: a
 * bounded, host-recoverable error leaves state.context reusable, so a
 * legal deep-but-smaller call right after the overflow must not still see
 * the limit, or any other, exhausted. */
static void test_recursion_depth(void)
{
	struct kg_lisp_arena_stats before, after;

	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(defun deep (n) (if (<= n 0) 0 (+ 1 (deep (- n 1)))))"));
	CHECK(eval_eq("(deep 200)", "200"));
	CHECK(kg_lisp_arena_stats(&before) == 0);
	/* 200 real recursion levels already cost the majority of a
	 * default-sized arena's frame capacity; confirms the 3-frames-per-
	 * level shape this comment's derivation relies on stays in that
	 * ballpark rather than silently becoming O(1) or O(N^2). */
	CHECK(before.peak_frame_depth > 200 * 2);
	CHECK(before.peak_frame_depth < before.frame_capacity);

	CHECK(eval_error_contains(
	    "(deep 1000000)", "evaluation frame limit exceeded"));
	CHECK(kg_lisp_arena_stats(&after) == 0);
	CHECK(after.allocation_failures == 0);
	CHECK(after.peak_frame_depth == after.frame_capacity);

	CHECK(eval_eq("(deep 200)", "200"));
	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
}

/* Exhaustion of the fixed arena, from kg's own evaluator entry point.
 *
 * Sub-plan 09D Part 3, and the kg half of 09A's Table X.  Fe sub-plan 09B
 * pre-builds `(arena-exhaustion)` and `(evaluation-stack-exhaustion)` at
 * FeOpenContext and keeps them rooted, so signalling one allocates
 * nothing -- which is what makes an out-of-memory catchable by a handler
 * that has to *name* a condition.  Before that pin only `(t ...)` caught
 * it: the raise degraded the condition object to nil and ConditionMatches,
 * which walks a pair to reach the hierarchy, answered false for every
 * named handler.  This asserts the new contract through
 * kg_lisp_eval_string(), the same entry point M-: and an init file use,
 * rather than through fe's own test harness.
 *
 * There is no Emacs oracle for any of this and there deliberately is not
 * one: Emacs has no arena, so `comparison: kg-policy` is what
 * test/lisp-compat/features.json's `arena-exhaustion-catchable-by-name`
 * row records.
 *
 * Three things it pins that must NOT change with it: a Budget wall (the
 * step limit) is still caught by nothing, not even `(t ...)`, per 09A
 * Decision 2; a chain built into a `let` local is collectable again the
 * moment the handler runs, so the session recovers fully; and a chain
 * built into a *global* keeps the arena pinned afterwards -- 09A
 * Decision 3's "recorded, not rescued", which doc/TODO.md carries as the
 * reset-command debt.  The final kg_lisp_shutdown() is the exhausted
 * session's teardown; the suite runs under the sanitizer lanes' leak
 * checkers, so "it shut down without leaking" needs no separate gate. */
static void test_arena_exhaustion_conditions(void)
{
	/* Built into a let-local, so the chain is unreachable the instant
	 * the handler frame is entered and the handler is free to allocate. */
	static const char fill_local[]
	    = "(let ((l nil)) (while t (setq l (cons 1 l))))";
	struct kg_lisp_arena_stats fresh, caught, pinned;
	char form[256];

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_arena_stats(&fresh) == 0);
	CHECK(fresh.allocation_failures == 0);
	CHECK(fresh.free_slots * 2 > fresh.total_slots);

	/* 1. `(t ...)` -- the one handler shape that worked before the pin. */
	snprintf(form, sizeof(form), "(condition-case e %s (t 'caught))",
	    fill_local);
	CHECK(eval_eq(form, "caught"));
	CHECK(kg_lisp_arena_stats(&caught) == 0);
	CHECK(caught.allocation_failures == fresh.allocation_failures + 1);
	CHECK(caught.total_slots == fresh.total_slots);
	CHECK(caught.free_slots > 0);
	/* Ordinary evaluation between rounds: the session is not damaged. */
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 2. `(error ...)` -- the generic named handler, which is what an
	 * init file or a package actually writes.  This is the assertion
	 * that would have failed at every pin before this one. */
	snprintf(form, sizeof(form), "(condition-case e %s (error 'caught))",
	    fill_local);
	CHECK(eval_eq(form, "caught"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 3. The specific name, and what the handler's variable is bound to:
	 * the pre-built condition object, not nil. */
	snprintf(form, sizeof(form),
	    "(condition-case e %s (arena-exhaustion 'caught))", fill_local);
	CHECK(eval_eq(form, "caught"));
	snprintf(
	    form, sizeof(form), "(condition-case e %s (error e))", fill_local);
	CHECK(eval_eq(form, "(arena-exhaustion)"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 4. The hierarchy is a real answer, not a catch-all: an unrelated
	 * named handler still lets the exhaustion through to the host, and
	 * the host still renders the same message text it always did. */
	snprintf(form, sizeof(form),
	    "(condition-case e %s (arith-error 'caught))", fill_local);
	CHECK(eval_error_contains(form, "out of memory"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 5. Budget stays uncatchable (09A Decision 2): the step wall is not
	 * a condition, and `(t ...)` does not see it. */
	CHECK(eval_error_contains(
	    "(condition-case e (while t 1) (t 'caught))", "step limit"));
	CHECK(eval_eq("(+ 1 2)", "3"));

	/* 6. Rooted exhaustion is recorded, not rescued (09A Decision 3),
	 * and the measured answer is sharper than "it is not rescued": the
	 * handler does not even run.  The same chain built into a *global*
	 * is still reachable when the raise happens, so the collector has
	 * nothing to give back, and entering a `condition-case` handler that
	 * binds a variable is itself an allocation -- fe's handler-re-entry
	 * rule (09B) says a handler that cannot allocate re-raises, and with
	 * no enclosing handler the re-raise reaches the host.  So this is the
	 * one exhaustion shape in this test that is *reported* rather than
	 * caught, and the report is the same text as always.
	 *
	 * Measured here: rc 1, "eval:1: out of memory", free_slots 0,
	 * allocation_failures 7. */
	CHECK(eval_ok("(setq exhaustion-global nil)"));
	CHECK(eval_error_contains("(condition-case e"
				  " (while t (setq exhaustion-global"
				  " (cons 1 exhaustion-global)))"
				  " (error 'caught))",
	    "out of memory"));
	CHECK(kg_lisp_arena_stats(&pinned) == 0);
	CHECK(pinned.total_slots == fresh.total_slots);
	CHECK(pinned.free_slots * 100 < pinned.total_slots);
	CHECK(pinned.allocation_failures > caught.allocation_failures);

	/* 7. ... and the session is alive, correctly reporting, and useless
	 * -- 09A's three words for this state, each asserted.  Ordinary
	 * evaluation still answers, and a handler still catches an ordinary
	 * error, because the collector can still recycle what the *new* form
	 * allocates; what it cannot do is return the pinned chain.  This is
	 * the state M-x lisp-arena-stats exists to make visible, and the
	 * reset command doc/TODO.md records is what would end it. */
	CHECK(eval_eq("(+ 1 2)", "3"));
	CHECK(eval_eq("(condition-case nil (car 1) (error 'ok))", "ok"));
	CHECK(kg_lisp_arena_stats(&pinned) == 0);
	CHECK(pinned.free_slots * 100 < pinned.total_slots);

	kg_lisp_shutdown();

	/* And a fresh context after an exhausted one is a fresh context. */
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_arena_stats(&fresh) == 0);
	CHECK(fresh.allocation_failures == 0);
	CHECK(fresh.free_slots * 2 > fresh.total_slots);
	CHECK(eval_eq("(+ 1 2)", "3"));
	kg_lisp_shutdown();

	/* 8. The arena is one of the two exhaustions Phase 9 named; this is
	 * the other.  README.md and doc/lisp-api.md both tell a user that a
	 * full GC root stack is catchable as `evaluation-stack-exhaustion`,
	 * and nothing on kg's side of the pin asserted it -- only fe's own
	 * test_api.c did, and that one provokes the overflow from a native
	 * written for the purpose.  This reaches it through kg's ordinary
	 * entry point instead.
	 *
	 * The route is the *reader*, not the evaluator: an evaluation deep
	 * enough to fill the 4096-slot root stack hits the 1095-frame wall
	 * first and raises Budget, which is uncatchable by design (case 5
	 * above).  Reading a datum nested `gc_stack_deep` levels pushes a
	 * root per level with no frame at all, so `(load FILE)` -- whose
	 * read happens at run time, inside the enclosing handler's extent --
	 * is what puts the overflow where a handler can see it.  Measured on
	 * this build: the same file loaded without a handler reports
	 * `<path>:1: GC stack overflow`, and the depth is chosen well past
	 * 4096 rather than at it, since the reader is not the only thing on
	 * that stack. */
	{
		char path[] = "/tmp/kg-lisp-deep-XXXXXX";
		static constexpr int gc_stack_deep = 8192;
		int fd = mkstemp(path);
		FILE *file = fd >= 0 ? fdopen(fd, "w") : nullptr;
		int i;

		CHECK(file != nullptr);
		if (file == nullptr) {
			if (fd >= 0) {
				(void)close(fd);
				(void)unlink(path);
			}
			return;
		}
		(void)fputc('\'', file);
		for (i = 0; i < gc_stack_deep; i++) {
			(void)fputc('(', file);
		}
		(void)fputc('1', file);
		for (i = 0; i < gc_stack_deep; i++) {
			(void)fputc(')', file);
		}
		CHECK(fclose(file) == 0);

		CHECK(kg_lisp_init() == 0);
		snprintf(form, sizeof(form),
		    "(condition-case nil (load \"%s\")"
		    " (evaluation-stack-exhaustion 'caught))",
		    path);
		CHECK(eval_eq(form, "caught"));
		snprintf(form, sizeof(form),
		    "(condition-case e (load \"%s\") (error e))", path);
		CHECK(eval_eq(form, "(evaluation-stack-exhaustion)"));
		snprintf(form, sizeof(form), "(load \"%s\")", path);
		CHECK(eval_error_contains(form, "GC stack overflow"));
		CHECK(eval_eq("(+ 1 2)", "3"));
		kg_lisp_shutdown();
		CHECK(unlink(path) == 0);
	}
}

/* Sub-plan 01A's second test path: lisp/prelude.el is the canonical source
 * and src/lisp_prelude_generated.inc is a generated copy of it that kg
 * evaluates at startup.  `make lisp-prelude-check` proves those two files
 * agree byte for byte; this proves the *file* and the *running editor*
 * agree definition for definition, and that the definitions are in the
 * order the prelude's first rule requires.
 *
 * It deliberately does not re-load the file into a booted context.  A
 * second evaluation is not idempotent, for exactly the reason rule 1
 * exists: `(defalias 'internal--let (symbol-function 'let))` captures Fe's
 * raw `let` primitive before the Emacs `let` macro shadows that name, so
 * re-running it where the macro already exists would bind internal--let to
 * the macro and break every list-library function that uses it.  That
 * hazard is what the type assertions below detect -- internal--let must
 * answer `primitive`, not `macro`, so a generator that ever reordered or
 * dropped forms fails here rather than silently shipping a broken prelude.
 *
 * Arity is not asserted separately: kg's Lisp surface has no func-arity
 * and a lambda's printed form carries no parameter list, so each
 * definition's type plus the byte-identity check above is what is
 * actually observable.
 *
 * Sub-plan 02D's dialect cutover deleted the kg-owned `setq` macro (built
 * on assignment `=`) and rewrote the remaining 53 definitions from `=' to
 * core `setq'; the scan below looked for column-zero "(setq NAME " forms.
 * Sub-plan 04E's Lisp-2 cut retargeted the function cell: the forms are
 * column-zero "(defalias 'NAME ...)" now, the deleted identity-lambda
 * `function` alias dropped the count to 52, and the type assertion reads
 * the function cell -- `(type-of (symbol-function 'NAME))` -- because the
 * names live there, not in the value namespace. */
static void test_condition_case_native_error(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(goto-char 5)"));
	/* goto-char with invalid string type raises wrong-type-argument;
	 * condition-case catches it, point stays at 5, buffer survives. */
	CHECK(eval_eq(
	    "(condition-case e (goto-char \"x\") (error 'caught))", "caught"));
	CHECK(eval_eq("(point)", "5"));
	CHECK(eval_eq("(buffer-name)", "bridge.txt"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_catch_throw_unwind(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* throw unwinds through unwind-protect cleanups in innermost-first
	 * order */
	CHECK(eval_ok("(setq trail nil)"));
	CHECK(eval_eq("(catch 'exit "
		      "  (unwind-protect "
		      "    (unwind-protect "
		      "      (throw 'exit 'done) "
		      "      (setq trail (cons 'inner trail))) "
		      "    (setq trail (cons 'outer trail))))",
	    "done"));
	CHECK(eval_eq("trail", "(outer inner)"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Sub-plan 06E.  save-excursion and with-current-buffer run a body between
 * a save and a restore, and both used FeCall to do it -- which starts a
 * *nested* evaluator run whose completions transfer to kg's own outermost
 * barrier, past every condition-case that lexically encloses the form.  The
 * protected call plus FeResignal (lisp_call_body) puts the completion back
 * in flight in the enclosing run instead, which is what these pin.
 *
 * `throw` used to be the deliberate exception, pinned here as what it was
 * rather than as what Emacs answers: fe walls the throw search at a native
 * re-entry boundary by design, so a throw out of either body found no catch
 * and became `(no-catch TAG VALUE)`.  Sub-plan 11D Part 4 closed it by
 * removing the native frame -- both forms are prelude macros over Lisp
 * `unwind-protect` now -- so the two throw blocks below assert the reverse:
 * the catch receives the thrown value, and the restore still ran on that
 * path.  This test remains the condition-case guard the 11D contract names:
 * the transparency 06E bought must survive the change of shape.  What is
 * still a wall is every native re-entry that is NOT one of these two forms
 * -- hooks (test_hook_throw_containment), process callbacks, a nested
 * command-execute -- because those are callbacks kg invokes from its own C
 * and no prelude expansion removes the frame. */
static void test_wrapping_native_transparency(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* 1. An error inside save-excursion reaches the enclosing
	 * condition-case, and the restore has already run when it does. */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_eq("(condition-case e"
		      "   (save-excursion (goto-char 8) (error \"boom\"))"
		      "   (error 'caught))",
	    "caught"));
	CHECK(eval_eq("(point)", "3"));

	/* 2. The handler sees the original condition, not a re-wrapped one. */
	CHECK(eval_eq("(condition-case e"
		      "   (save-excursion (car 1))"
		      "   (wrong-type-argument 'by-symbol))",
	    "by-symbol"));

	/* 3. A throw out of the body reaches the catch outside the form --
	 * with the value it threw, not a no-catch error -- and the restore
	 * ran on that path too.  The condition-case is left wrapped around
	 * it so this reads as the inversion of the assertion it replaces:
	 * the handler is simply never entered now. */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_eq("(condition-case e"
		      "   (catch 'tag"
		      "     (save-excursion (goto-char 8) (throw 'tag 'gone)))"
		      "   (error (car e)))",
	    "gone"));
	CHECK(eval_eq("(point)", "3"));

	/* 4. with-current-buffer answers the same three ways, and restores
	 * the selected buffer on each of them. */
	CHECK(eval_ok("(setq b2 (get-buffer-create \"wrap2\"))"));
	CHECK(eval_eq("(condition-case e"
		      "   (with-current-buffer b2 (error \"boom\"))"
		      "   (error 'caught))",
	    "caught"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));
	CHECK(eval_eq("(condition-case e"
		      "   (catch 'tag (with-current-buffer b2"
		      "     (throw 'tag 'gone)))"
		      "   (error (car e)))",
	    "gone"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	/* 5. A body that completes normally is unaffected by any of this. */
	CHECK(eval_eq("(with-current-buffer b2 (buffer-name))", "wrap2"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	/* 6. The quit path, which no compat case can reach: C-g out of the
	 * body still restores.  An `unwind-protect' cleanup runs on every
	 * completion kind fe has, and this is the one that is neither a
	 * return, an error nor a throw. */
	{
		char result[128] = "";
		static const char quitting[]
		    = "(save-excursion (goto-char 8) (while t 1))";

		CHECK(eval_ok("(goto-char 3)"));
		interrupt_polls = 0;
		kg_lisp_set_interrupt_check(cancel_evaluation);
		CHECK(kg_lisp_eval_string(quitting, sizeof(quitting) - 1,
			  result, sizeof(result))
		    != 0);
		kg_lisp_set_interrupt_check(nullptr);
		CHECK(strcmp(result, "Quit") == 0);
		CHECK(eval_eq("(point)", "3"));

		CHECK(eval_ok("(set-buffer (get-buffer \"bridge.txt\"))"));
		interrupt_polls = 0;
		kg_lisp_set_interrupt_check(cancel_evaluation);
		CHECK(!eval_ok("(with-current-buffer b2 (while t 1))"));
		kg_lisp_set_interrupt_check(nullptr);
		CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));
	}

	kg_lisp_shutdown();
	teardown_editor();
}

/* Sub-plan 06E, and the honest half of the compat manifest's
 * condition-case-native-errors row.  kg's own editor natives still raise
 * prose through FeHandleError -- 06A's Decision 2 deferred classifying them
 * -- so the condition they carry is plain `error` whose message happens to
 * read "wrong-type-argument".  A generic handler therefore catches them and
 * a handler naming the specific symbol does not, which is a divergence from
 * Emacs and is written down as one. */
static void test_condition_case_kg_native_conditions(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* A generic (error ...) handler catches a kg native's raise. */
	CHECK(eval_eq("(condition-case e (goto-char \"x\") (error 'generic))",
	    "generic"));
	/* The symbol the *message* names does not match, because the
	 * condition object is `(error "wrong-type-argument")`. */
	CHECK(eval_error_contains(
	    "(condition-case e (goto-char \"x\") (wrong-type-argument 'sym))",
	    "wrong-type-argument"));
	CHECK(eval_eq(
	    "(condition-case e (goto-char \"x\") (error (car e)))", "error"));
	/* Fe's own natives, by contrast, already carry classified
	 * conditions -- so this is a kg-side gap, not a Lisp-wide one. */
	CHECK(eval_eq(
	    "(condition-case e (car 1) (wrong-type-argument 'sym))", "sym"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* Sub-plan 06E.  Hook containment swallows a hook's *error* and carries on
 * with the next hook; it must not swallow a quit, or C-g would be eaten by
 * whichever hook happened to be running when it arrived.  The quit is put
 * back in flight with FeResignal and cancels the whole evaluation. */
static void test_hook_quit_is_not_contained(void)
{
	char result[128] = "";
	static const char *src = "(progn (run-hooks 'spin-hook) 'survived)";

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq after-ran nil)"));
	CHECK(eval_ok("(add-hook 'spin-hook (lambda () (while t 1)))"));
	CHECK(eval_ok("(add-hook 'spin-hook (lambda () (setq after-ran t)))"));

	interrupt_polls = 0;
	kg_lisp_set_interrupt_check(cancel_evaluation);
	CHECK(
	    kg_lisp_eval_string(src, strlen(src), result, sizeof(result)) != 0);
	kg_lisp_set_interrupt_check(nullptr);
	/* Reported as the cancellation it is, not as a hook error, and the
	 * hooks after it never ran. */
	CHECK(strcmp(result, "Quit") == 0);
	CHECK(strstr(test_status_message, "Hook error") == nullptr);
	CHECK(eval_eq("after-ran", "nil"));
	/* The interpreter is still usable afterwards. */
	CHECK(eval_eq("(+ 1 1)", "2"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_hook_throw_containment(void)
{
	char result[128] = "";
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(
	    eval_ok("(add-hook 'my-test-hook (lambda () (throw 'tag 'val)))"));
	const char *src = "(catch 'tag (run-hooks 'my-test-hook))";
	int rc = kg_lisp_eval_string(src, strlen(src), result, sizeof(result));

	/* The hook's throw cannot reach the catch outside (run-hooks): the
	 * protected call's barrier is a catch wall, so the throw finds no
	 * catch inside the hook's own run and is contained as no-catch.  The
	 * enclosing evaluation survives -- it is the whole point of hook
	 * containment -- and the status line says which hook failed and
	 * why. */
	CHECK(rc == 0);
	CHECK(strcmp(result, "nil") == 0);
	CHECK(strstr(test_status_message, "Hook error (my-test-hook)")
	    != nullptr);
	CHECK(strstr(test_status_message, "no-catch") != nullptr);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_signal(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* signal arith-error caught by arith-error handler */
	CHECK(eval_eq("(condition-case e (signal 'arith-error '(1)) "
		      "(arith-error (car e)))",
	    "arith-error"));

	/* signal arith-error caught by parent error handler */
	CHECK(eval_eq(
	    "(condition-case e (signal 'arith-error '(1)) (error (car e)))",
	    "arith-error"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_ignore_errors(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* ignore-errors returns value on success, nil on error */
	CHECK(eval_eq("(ignore-errors (+ 1 2))", "3"));
	CHECK(eval_eq("(ignore-errors (/ 1 0))", "nil"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_condition_case_reentry(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* condition-case inside hook inside condition-case */
	CHECK(eval_ok("(add-hook 'before-save-hook "
		      "  (lambda () (condition-case nil (/ 1 0) (error "
		      "'hook-caught))))"));
	CHECK(eval_eq("(condition-case nil (progn (run-hooks "
		      "'before-save-hook) 'ok) (error 'outer-caught))",
	    "ok"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The in-flight condition survives a containment inside a cleanup drain.
 * kg's `run-hooks' contains each hook function's completion (a failing
 * hook is reported, not propagated), and that containment publishes the
 * contained condition in the very context field the completion being
 * unwound is using.  When the drain is an `unwind-protect' cleanup that
 * kg's own error is passing through, an enclosing `condition-case' used to
 * bind the HOOK's condition object instead of the body's -- an error whose
 * message said one thing and whose object said another.  Fixed in fe at
 * this pin ("Hold the completion in flight across a cleanup drain"); this
 * is kg's half of the evidence, since kg is where a containment inside a
 * cleanup is reachable from ordinary Lisp. */
static void test_cleanup_containment_keeps_condition(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(add-hook 'before-save-hook (lambda () (car 6)))"));
	/* Body raises arith-error; the cleanup contains a
	 * wrong-type-argument.  The handler must see the body's. */
	CHECK(eval_eq("(condition-case e (unwind-protect (/ 1 0) "
		      "(run-hooks 'before-save-hook)) (error (car e)))",
	    "arith-error"));
	/* The cleanup still runs, and the contained hook error is still
	 * contained rather than escaping. */
	CHECK(eval_eq("(condition-case e (unwind-protect 'body "
		      "(run-hooks 'before-save-hook)) (error (car e)))",
	    "body"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_quit_uncaught(void)
{
	char result[128] = "";

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* signal quit is caught by quit handler, but NOT by error handler */
	CHECK(
	    eval_eq("(condition-case e (signal 'quit nil) (quit 'caught-quit))",
		"caught-quit"));

	const char *src
	    = "(condition-case e (signal 'quit nil) (error 'caught-error))";
	CHECK(
	    kg_lisp_eval_string(src, strlen(src), result, sizeof(result)) != 0);
	CHECK(strcmp(result, "Quit") == 0);

	kg_lisp_shutdown();
	teardown_editor();
}

#define PRELUDE_DEFS 77

static void test_prelude_source_file(void)
{
	static char names[PRELUDE_DEFS][64];
	static char types[PRELUDE_DEFS][16];
	static char text[65536];
	const char *paths[] = { "lisp/prelude.el", "../lisp/prelude.el" };
	FILE *fp = nullptr;
	size_t len = 0, count = 0, i, let_index = PRELUDE_DEFS;
	char *line;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fp = fopen(paths[i], "r");
		if (fp != nullptr) {
			break;
		}
	}
	CHECK(fp != nullptr);
	if (fp == nullptr) {
		return;
	}
	len = fread(text, 1, sizeof(text) - 1, fp);
	fclose(fp);
	text[len] = '\0';
	/* The whole file has to have fit, or the scan below reports a short
	 * count for a reason that has nothing to do with the prelude. */
	CHECK(len > 0 && len < sizeof(text) - 1);

	/* A definition is "(defalias 'NAME " in column 0; every continuation
	 * line in the file is indented, so nothing nested is ever found. */
	for (line = text; line != nullptr; line = strchr(line, '\n')) {
		const char *name_start, *value_start;
		size_t name_len;

		if (line[0] == '\n') {
			line++;
		}
		if (strncmp(line, "(defalias '", 11) != 0) {
			continue;
		}
		CHECK(count < PRELUDE_DEFS);
		if (count >= PRELUDE_DEFS) {
			break;
		}
		name_start = line + 11;
		name_len = strcspn(name_start, " )\n");
		CHECK(name_len > 0 && name_len < sizeof(names[0]));
		memcpy(names[count], name_start, name_len);
		names[count][name_len] = '\0';

		/* The declared shape decides the type the editor must report:
		 * a lambda form is a lambda, a macro form is a macro, and the
		 * four (symbol-function 'PRIM) forms are aliases of Fe
		 * primitives. */
		value_start = name_start + name_len;
		value_start += strspn(value_start, " ");
		if (strncmp(value_start, "(lambda", 7) == 0) {
			strcpy(types[count], "lambda");
		} else if (strncmp(value_start, "(macro", 6) == 0) {
			strcpy(types[count], "macro");
		} else if (strncmp(value_start, "(symbol-function '", 18)
		    == 0) {
			strcpy(types[count], "primitive");
		} else {
			/* Not a fallback: those three are the whole
			 * vocabulary this file defines things with, and a
			 * fourth shape has to be classified deliberately
			 * rather than silently called a primitive. */
			fprintf(stderr,
			    "prelude definition '%s' has an unrecognised value "
			    "shape: %.40s\n",
			    names[count], value_start);
			CHECK(0);
			break;
		}

		if (strcmp(names[count], "let") == 0) {
			let_index = count;
		}
		count++;
	}

	/* The count is pinned: a definition added or lost without an
	 * accompanying manifest entry is 00C's checker's business, but one
	 * added or lost without anyone noticing at all is this test's. */
	CHECK(count == PRELUDE_DEFS);

	/* Rule 1, structurally: the primitive alias comes first, and the
	 * macro that shadows the name it aliased comes after it. */
	CHECK(strcmp(names[0], "internal--let") == 0);
	CHECK(let_index > 0 && let_index < PRELUDE_DEFS);
	CHECK(strcmp(types[let_index], "macro") == 0);

	CHECK(kg_lisp_init() == 0);
	for (i = 0; i < count; i++) {
		char source[128];

		/* The definitions live in function cells, so the type probe
		 * reads the cell: a bare name in value position is void now. */
		(void)snprintf(source, sizeof(source),
		    "(type-of (symbol-function '%s))", names[i]);
		if (!eval_eq(source, types[i])) {
			fprintf(stderr,
			    "prelude definition %zu (%s): expected type %s\n",
			    i, names[i], types[i]);
			CHECK(0);
		}
	}
	/* Rule 1 again, behaviourally this time: internal--let is still Fe's
	 * one-binding primitive, so the list library it carries still works. */
	CHECK(eval_eq("(reverse '(1 2 3))", "(3 2 1)"));

	kg_lisp_shutdown();
}

/* 08A Table C, as GNU Emacs 31.0.90 answers it (/opt-3/emacs-31-lucid,
 * emacs -Q --batch).  fe core owns the refusals; kg's prelude owns the
 * `let' half, because a `let' compiles into a lambda application and fe
 * lets a lambda parameter named `t' shadow -- which Emacs also does for a
 * lambda, and does NOT do for a `let'. */
static void test_phase8_constants_and_keywords(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(condition-case e (setq t nil)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant t)"));
	CHECK(eval_eq("(condition-case e (setq nil 1)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant nil)"));
	CHECK(eval_eq("(condition-case e (setq :kw 1)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant :kw)"));
	CHECK(eval_eq("(condition-case e (let ((t 1)) t)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant t)"));
	CHECK(eval_eq("(condition-case e (let ((nil 1)) 1)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant nil)"));
	CHECK(eval_eq("(condition-case e (let ((:kw 1)) 1)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant :kw)"));
	/* let* goes through the same binding-name helper. */
	CHECK(eval_eq("(condition-case e (let* ((t 1)) t)"
		      " (setting-constant (list (car e) (car (cdr e)))))",
	    "(setting-constant t)"));
	/* An ordinary binding list still binds. */
	CHECK(eval_eq("(let ((a 1) (b 2)) (+ a b))", "3"));
	CHECK(eval_eq("(let* ((a 1) (b (+ a 1))) b)", "2"));
	/* A lambda parameter named t may shadow -- Emacs answers 1 too. */
	CHECK(eval_eq("((lambda (t) t) 1)", "1"));
	/* Keywords self-evaluate and answer keywordp. */
	CHECK(eval_eq("(list :foo (keywordp :foo) (keywordp 'foo) (eq :a ':a))",
	    "(:foo t nil t)"));
	/* `:' alone is a keyword too, which 08B first guessed it was not. */
	CHECK(eval_eq("(keywordp ':)", "t"));

	kg_lisp_shutdown();
}

/* 08A Table R.  Every accepted row is byte-identical to the pinned
 * oracle's answer; every rejected row is a named read error, which is
 * 08C's "reject rather than misread" rule made checkable. */
static void test_phase8_reader_literals(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(list ?a ?\\n ?\\t ?\\e ?\\\\ ?\\s ?\\d)",
	    "(97 10 9 27 92 32 127)"));
	CHECK(eval_eq("(list ?\\C-a ?\\M-a)", "(1 134217825)"));
	CHECK(eval_eq("?\xc3\xa9", "233"));
	/* The control modifier is not `& 0x1f': ? is DEL, @.._ and a..z
	 * fold, and everything else keeps its value with the 2^26 bit. */
	CHECK(eval_eq(
	    "(list ?\\C-? ?\\C-% ?\\C-\xc3\xa9)", "(127 67108901 67109097)"));
	CHECK(eval_eq("(list #x10 #o17 #b101 #xff)", "(16 15 5 255)"));
	CHECK(eval_eq("(list \"\\x41\" \"\\101\" \"\\e\" \"\\d\" \"\\s\")",
	    "(\"A\" \"A\" \"\x1b\" \"\x7f\" \" \")"));

	/* Rejections, by name.  Each of these used to be read as something
	 * else -- a vector as three symbols, ?ab as one character plus a
	 * leftover token, "\x41f" as "Af". */
	CHECK(eval_error_contains("(list [1 2 3])", "vector brackets"));
	CHECK(eval_error_contains("(list '#:sym)", "unsupported read syntax"));
	CHECK(eval_error_contains("(cdr '(a\\ b))", "symbol escape"));
	CHECK(eval_error_contains("(list \"\\q\")", "unknown escape"));
	CHECK(eval_error_contains("(list ?ab)", "? literal without delimiter"));
	CHECK(eval_error_contains("(list ?\\s-a)", "\\s character modifier"));
	CHECK(eval_error_contains("(list ?\\^a)", "\\^ character modifier"));
	CHECK(eval_error_contains(
	    "(list \"\\x41f\")", "character above 255 in string"));
	CHECK(
	    eval_error_contains("(list \"\\0a\")", "NUL character in string"));
	CHECK(eval_error_contains(
	    "(list ?\\x110000)", "\\x character out of range"));

	kg_lisp_shutdown();
}

static void test_phase8_library(void)
{
	struct kg_lisp_arena_stats before, after;

	CHECK(kg_lisp_init() == 0);
	CHECK(eval_eq("(assq 'a '((a . 1) (b . 2)))", "(a . 1)"));
	CHECK(eval_eq("(mapc (lambda (x) x) '(1 2 3))", "(1 2 3)"));
	CHECK(eval_eq(
	    "(mapconcat (lambda (x) x) '(\"1\" \"2\" \"3\") \",\")", "1,2,3"));
	/* SEPARATOR is optional since Emacs 29 and defaults to "". */
	CHECK(eval_eq("(mapconcat (lambda (x) x) '(\"a\" \"b\"))", "ab"));
	CHECK(eval_eq(
	    "(mapconcat 'number-to-string '(1 2 3) \", \")", "1, 2, 3"));
	/* number-to-string: Emacs' integer and float syntaxes, which are
	 * fe's writer's, and wrong-type-argument for a non-number. */
	CHECK(eval_eq("(number-to-string 3)", "3"));
	CHECK(eval_eq("(number-to-string 1.0)", "1.0"));
	CHECK(eval_eq("(number-to-string 1.5)", "1.5"));
	CHECK(eval_eq("(number-to-string -0.0)", "-0.0"));
	CHECK(eval_error_contains(
	    "(number-to-string \"x\")", "wrong-type-argument"));
	/* string-to-list: codepoints, not bytes, and nil for "". */
	CHECK(eval_eq("(string-to-list \"ab\xc3\xa9\")", "(97 98 233)"));
	CHECK(eval_eq("(string-to-list \"\")", "nil"));
	/* mapc is distinguishable from mapcar only through a side effect:
	 * it answers its LIST argument, not the collected results. */
	CHECK(eval_eq("(let ((acc nil))"
		      " (list (mapc (lambda (x) (setq acc (cons x acc)))"
		      " '(1 2 3)) acc (mapcar (lambda (x) (* x 2)) '(1 2 3))))",
	    "((1 2 3) (3 2 1) (2 4 6))"));
	CHECK(eval_eq("(progn (setq x (list 1 2 3)) (nreverse x))", "(3 2 1)"));
	/* nreverse mutates: the name still points at what was the head, so
	 * it is now the one-element tail.  Emacs answers ((2 1) (1)). */
	CHECK(eval_eq(
	    "(let ((x (list 1 2))) (list (nreverse x) x))", "((2 1) (1))"));
	CHECK(eval_eq("(delq 2 (list 1 2 1))", "(1 1)"));
	CHECK(eval_eq("(delete '(a) (list '(a) '(b)))", "((b))"));
	/* delq compares with eq and delete with equal, which is the whole
	 * difference: the same freshly consed needle is found by one and
	 * not the other.  Emacs answers (((1) (2)) ((2))). */
	CHECK(eval_eq("(let ((needle (list 1)) (items (list (list 1)"
		      " (list 2)))) (list (delq needle items)"
		      " (delete needle (list (list 1) (list 2)))))",
	    "(((1) (2)) ((2)))"));
	/* The head-removal contract: the result must be assigned back,
	 * because removing the first element cannot be done in place. */
	CHECK(eval_eq(
	    "(progn (setq x (list 1 2)) (setq x (delq 1 x)) x)", "(2)"));
	/* A cond clause with no body answers its own test's value. */
	CHECK(eval_eq("(list (cond (5)) (cond (nil 1) (2)) (cond (nil 1))"
		      " (cond))",
	    "(5 2 nil nil)"));
	CHECK(
	    eval_eq("(progn (setq x '(2 1)) (add-to-list 'x 3) x)", "(3 2 1)"));
	/* APPEND puts the new element last, and an element already there
	 * (by `equal') leaves the list alone. */
	CHECK(eval_eq(
	    "(progn (setq x (list 2 1)) (add-to-list 'x 3 t))", "(2 1 3)"));
	CHECK(eval_eq(
	    "(progn (setq x (list 1 2)) (add-to-list 'x 1) x)", "(1 2)"));
	/* Emacs' add-to-list is a function taking a symbol, so the symbol
	 * argument is evaluated: this reaches my-list, not `s'.  kg's was a
	 * macro pattern-matching a literal (quote NAME) and answered
	 * (1) here, having assigned to `s'. */
	CHECK(eval_eq("(let ((s 'my-list)) (setq my-list (list 1))"
		      " (add-to-list s 2) my-list)",
	    "(2 1)"));
	CHECK(eval_eq("(identity 7)", "7"));
	CHECK(eval_eq("(prog2 1 2 3)", "2"));
	CHECK(eval_eq("(max 1 4 2)", "4"));
	CHECK(eval_eq("(min 1 4 2)", "1"));
	/* Emacs answers (wrong-number-of-arguments max 0) for a bare call
	 * and (wrong-type-argument ...) for a non-number operand; kg used
	 * to answer a prose error for the first. */
	CHECK(eval_error_contains("(max)", "wrong-number-of-arguments"));
	CHECK(eval_error_contains("(min)", "wrong-number-of-arguments"));
	CHECK(eval_error_contains("(max \"a\" 1)", "wrong-type-argument"));
	CHECK(eval_error_contains("(min \"a\" 1)", "wrong-type-argument"));
	CHECK(eval_eq("(progn (makunbound 'no-value) (defvar no-value) "
		      "(boundp 'no-value))",
	    "nil"));
	CHECK(eval_eq("(progn (defun documented () \"A doc.\" nil) "
		      "(documentation 'documented))",
	    "A doc."));
	CHECK(eval_eq("(progn (defvar documented-var 1 \"A variable.\") "
		      "(documentation 'documented-var))",
	    "A variable."));
	CHECK(eval_eq("(progn (defconst documented-const 1 \"A constant.\") "
		      "(documentation 'documented-const))",
	    "A constant."));
	/* (defvar SYMBOL &optional VALUE DOCSTRING): a lone string is the
	 * VALUE, not a docstring.  Emacs 31.0.90 answers "hello" for the
	 * first row; kg left dv1 unbound because a single string argument
	 * was classified as documentation and no setq was emitted.  Only
	 * the second element is ever documentation. */
	CHECK(eval_eq(
	    "(progn (makunbound 'dv1) (defvar dv1 \"hello\") dv1)", "hello"));
	CHECK(eval_eq("(progn (makunbound 'dv2) (defvar dv2 \"v\" \"doc\")"
		      " (list dv2 (documentation 'dv2)))",
	    "(\"v\" \"doc\")"));
	CHECK(eval_eq(
	    "(progn (makunbound 'dv3) (defvar dv3) (boundp 'dv3))", "nil"));
	CHECK(eval_eq("(progn (makunbound 'dv4) (defvar dv4 1) dv4)", "1"));
	/* And the rule defvar exists for: an already-bound variable keeps
	 * its value, whatever the declaration says. */
	CHECK(eval_eq("(progn (setq dv5 9) (defvar dv5 1) dv5)", "9"));
	/* defmacro takes defun's lone-string rule -- a string is
	 * documentation only when a further form follows it -- and routes
	 * the docstring to internal--doc-put.  Emacs answers
	 * "just a string" and "doc" for these two. */
	CHECK(eval_eq("(progn (defmacro dm1 (x) \"just a string\") (dm1 3))",
	    "just a string"));
	CHECK(eval_eq("(progn (defmacro dm2 (x) \"doc\" (list '+ x 1))"
		      " (list (dm2 3) (documentation 'dm2)))",
	    "(4 \"doc\")"));
	CHECK(eval_eq("(progn (defmacro dm3 (x) (list '+ x 1)) (dm3 3))", "4"));
	CHECK(eval_eq("(progn (defmacro dm4 (x) \"d\" (declare (indent 0))"
		      " (list '+ x 1)) (list (dm4 3) (documentation 'dm4)))",
	    "(4 \"d\")"));
	CHECK(eval_eq("(progn (setq-default answer 8) answer)", "8"));
	CHECK(eval_eq("(progn (setq-local answer 9) answer)", "9"));
	CHECK(eval_eq("(kbd \"C-c k\")", "C-c k"));
	CHECK(eval_error_contains("(kbd \"M-x\")", "cannot bind key sequence"));
	CHECK(eval_eq("(progn (makunbound 'phase8-custom)"
		      " (defcustom phase8-custom (+ 2 3) \"custom doc\""
		      " :type 'integer :group 'editing))",
	    "phase8-custom"));
	CHECK(eval_eq("phase8-custom", "5"));
	CHECK(eval_eq("(documentation 'phase8-custom)", "custom doc"));
	CHECK(eval_ok(
	    "(progn (setq phase8-custom 9)"
	    " (defcustom phase8-custom (error \"evaluated\") \"doc\"))"));
	CHECK(eval_eq("phase8-custom", "9"));
	CHECK(eval_ok("(custom-set-variables '(phase8-custom 11))"));
	CHECK(eval_eq("phase8-custom", "11"));
	CHECK(eval_error_contains(
	    "(custom-set-variables '(phase8-custom 12 :type integer))",
	    "entry must be"));
	CHECK(eval_error_contains(
	    "(defcustom phase8-bad 1 \"doc\" :initialize t)",
	    "semantic keyword"));
	CHECK(eval_error_contains(
	    "(defcustom phase8-bad 1 \"doc\" :unknown t)", "unknown keyword"));
	CHECK(eval_error_contains(
	    "(defcustom phase8-bad 1 \"doc\" :type)", "keyword tail"));
	CHECK(!eval_ok("(custom-set-variables '(\"x\" 12))"));
	CHECK(eval_eq("(progn (defun phase8-declared () \"doc\""
		      " (declare (something ignored)) 7) (phase8-declared))",
	    "7"));
	CHECK(eval_eq("(progn (defmacro phase8-declared-macro (x)"
		      " \"doc\" (declare (something ignored)) x)"
		      " (phase8-declared-macro 8))",
	    "8"));
	/* What the Phase 8 prelude batch costs the fixed arena.  The
	 * assertion this replaces -- after.peak_live >= before.peak_live --
	 * was a tautology: peak_live_objects is a high-water mark, so it can
	 * never fall.  The claim worth making is margin, the same one
	 * test_perf.c's prelude case makes: after the whole prelude and
	 * every form above it, more than half the arena is still free and
	 * the high-water mark is a small fraction of it.  Re-measured on this
	 * build via kg_lisp_arena_stats() at the Phase 11 pin: 56226 object
	 * slots (56224 at the Phase 10 pin, 56222 before that -- the frame
	 * record fe's dynamic-binding work grew costs one frame slot,
	 * frame_capacity 1096 -> 1095, and returns two object slots),
	 * peak_live 5227 after the prelude alone (5210 at the Phase 10 pin,
	 * 5188 at Phase 9's), and 8159 at this point in this function, after
	 * every Phase 8 form above -- 14.5% of the arena for everything kg
	 * ships plus this test's own corpus.  The two live figures moved by
	 * exactly +17 each across this pin: fe's two new primitives
	 * (internal--mark-special, special-variable-p) and their interned
	 * names, plus the prelude's own switch onto the core `let'.  Both
	 * re-measured by instrumenting this exact line, not derived -- the
	 * "8230" this comment used to carry never reproduced, and the rule
	 * that replaced it is that every number here is measured at the pin
	 * that records it.  The Phase 10 comment also carried "5751 with
	 * lisp/auto-fill.el on top of it"; that figure is dropped rather
	 * than carried forward, because the method behind it was not
	 * recorded and evaluating that file's text in a fresh post-prelude
	 * context measures 5927 here, which is not the same measurement. */
	CHECK(kg_lisp_arena_stats(&before) == 0);
	CHECK(eval_ok("(mapconcat (lambda (x) x) "
		      "'(\"1\" \"2\" \"3\" \"4\" \"5\") \":\")"));
	CHECK(kg_lisp_arena_stats(&after) == 0);
	CHECK(after.free_slots * 2 > after.total_slots);
	CHECK(after.peak_live_objects * 4 < after.total_slots);
	CHECK(after.free_slots <= before.free_slots);
	CHECK(after.allocation_failures == 0);
	kg_lisp_shutdown();
}

static void test_save_excursion(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* 1. Body moves point, after return point is restored. */
	CHECK(eval_eq(
	    "(progn (goto-char 1) (save-excursion (goto-char 7)) (point))",
	    "1"));

	/* 2. Body errors, point is restored. */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_error_contains(
	    "(save-excursion (goto-char 8) (car 1))", "expected pair"));
	CHECK(eval_eq("(point)", "3"));

	/* 3. Body inserts before saved point, restoration follows the marker.
	 */
	CHECK(eval_ok("(goto-char 7)"));
	CHECK(eval_ok("(save-excursion (goto-char 1) (insert \"prefix \"))"));
	CHECK(eval_eq("(point)", "14"));

	/* 4. Body attempts to kill buffer (fails because modified). */
	CHECK(eval_error_contains(
	    "(save-excursion (kill-buffer (get-buffer \"bridge.txt\")))",
	    "modified"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* The adapter object pool bounds how many excursions are open AT ONCE,
 * not how many a run performs.  The Phase 11 acceptance review's BLOCKER
 * B1: 11D Part 4's capture mints a marker record out of the 64-entry
 * pool (LISP_MAX_OBJECTS) and the restore only detached it, so the record
 * stayed taken until fe's collector swept the wrapper -- which a loop
 * allocating almost no arena never provokes, the pool having no
 * back-pressure of its own.  The 65th `save-excursion' between two
 * collections raised "too many buffer objects", where the pre-Phase-11
 * malloc'd implementation ran 5000 of them.
 *
 * Every number below is the review's, so a re-introduction fails here
 * rather than in a user's init.el.  There is no test above this one that
 * calls `save-excursion' more than twice, which is why every gate was
 * green on the defect. */
static void test_save_excursion_pool_bound(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(kg_lisp_init() == 0);

	/* 1. The exact threshold: 64 records, so 65 was the first failure. */
	CHECK(eval_eq("(let ((i 0)) (while (< i 65) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "65"));

	/* 2. The old answer, restored: 5000 sequential excursions. */
	CHECK(eval_eq("(let ((i 0)) (while (< i 5000) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "5000"));

	/* 3. Records do not survive the evaluation that made them: this is a
	 * second, separate eval_eq(), and 40 + 40 > 64. */
	CHECK(eval_eq("(let ((i 0)) (while (< i 40) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "40"));
	CHECK(eval_eq("(let ((i 0)) (while (< i 40) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "40"));

	/* 4. The budget is shared with buffer objects, so the review's
	 * three-extra-buffers variant is its own case. */
	CHECK(eval_ok("(get-buffer-create \"pool1\")"));
	CHECK(eval_ok("(get-buffer-create \"pool2\")"));
	CHECK(eval_ok("(get-buffer-create \"pool3\")"));
	CHECK(eval_eq("(let ((i 0)) (while (< i 200) (save-excursion "
		      "(setq i (+ i 1)))) i)",
	    "200"));

	/* 5. Nesting is what the pool still bounds, and that bound is now
	 * 64 deep -- strictly deeper than the pre-Phase-11 native form
	 * managed, which re-entered the evaluator and so hit the
	 * 32-re-entry wall at 33.  Five deep here, because sixty-four
	 * nested forms in a C string literal test nothing this does not;
	 * the ceiling itself is a measurement, recorded in the commit. */
	CHECK(eval_eq("(save-excursion (save-excursion (save-excursion "
		      "(save-excursion (save-excursion 'deep)))))",
	    "deep"));

	/* 6. A released record is reusable, and the collector must not take
	 * the reuser's record with the dead wrapper it sweeps.  The marker
	 * below lands on a record some earlier excursion released; the cons
	 * loop then forces collections that sweep those dead excursion
	 * wrappers.  Without the wrapper-identity check in lisp_object_gc()
	 * this answers "marker-position: expected a marker". */
	CHECK(eval_ok("(goto-char 3)"));
	CHECK(eval_ok("(setq pm (make-marker))"));
	CHECK(eval_ok("(let ((j 0)) (while (< j 40000) (setq j (+ j 1)) "
		      "(cons j j)))"));
	CHECK(eval_eq("(marker-position pm)", "3"));

	/* 7. The tolerances the release must not cost: a second manual
	 * restore of a spent state is still harmless, and a value that is not
	 * an adapter object at all is still a type error. */
	CHECK(eval_eq("(let ((s (internal--excursion-capture t))) (list "
		      "(internal--excursion-restore s) "
		      "(internal--excursion-restore s)))",
	    "(nil nil)"));
	CHECK(eval_error_contains("(internal--excursion-restore 5)",
	    "expected a marker or a buffer"));
	CHECK(eval_error_contains("(internal--excursion-restore nil)",
	    "expected a marker or a buffer"));

	/* 8. And the point restoration itself still works after all of it. */
	CHECK(eval_eq(
	    "(progn (goto-char 2) (save-excursion (goto-char 7)) (point))",
	    "2"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_with_current_buffer(void)
{
	setup_editor();
	editor_insert_row(bcur(), 0, "main buffer", 11);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq b2 (get-buffer-create \"buf2\"))"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	/* 1. Body operates on non-displayed buffer, displayed window unchanged.
	 */
	CHECK(eval_ok("(with-current-buffer b2 (insert \"hidden content\"))"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));
	CHECK(eval_eq("(with-current-buffer b2 (buffer-substring 1 15))",
	    "hidden content"));

	/* 2. Body errors, context is restored. */
	CHECK(eval_error_contains(
	    "(with-current-buffer b2 (car 1))", "expected pair"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	/* 3. Buffer argument is dead/stale, error before body runs. */
	/* Note: buf2 is dirty after insert, so kill-buffer with optional buffer
	 * obj works or we clean it first */
	CHECK(eval_ok("(setq b3 (get-buffer-create \"buf3\"))"));
	CHECK(eval_ok("(kill-buffer b3)"));
	CHECK(eval_error_contains(
	    "(with-current-buffer b3 (insert \"hi\"))", "buffer is dead"));
	CHECK(eval_eq("(buffer-name (current-buffer))", "bridge.txt"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* A hook member that signals keeps its place; Emacs' would have been
 * removed (sub-plan 10D Part 1).
 *
 * Measured under /opt-3/emacs-31-lucid/bin/emacs 31.0.90 before this was
 * written, not read out of a manual: a member of `after-change-functions'
 * that signals is called exactly ONCE across three insertions -- the
 * error reaches the caller and Emacs empties the hook, so the two later
 * insertions are clean and the hook's value is nil afterwards.  kg
 * contains the error, reports `Hook error (...)', and leaves the member
 * armed, so the same shape calls it three times.
 *
 * Recorded, not fixed: which policy is right is a host design question
 * (Emacs' disarming is a modification-hook rule, not a general hook
 * rule, and kg's containment is what makes C-g and budget completions
 * survive a hook), and a package can take Emacs' policy for itself --
 * lisp/auto-fill.el does, with a condition-case that removes itself, and
 * test/pty/lisp-auto-fill-mode-error-disarms.yaml is the proof that
 * works.  This test is the divergence's own witness, cited by
 * test/lisp-compat/features.json's hook-error-does-not-disarm row.
 *
 * before-save-hook rather than after-change-functions: the counting is
 * the property, run-hooks is the direct way to run one three times, and
 * it needs no editing to do it. */
static void test_hook_error_does_not_disarm(void)
{
	int i;

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq disarm-calls 0)"));
	CHECK(eval_ok("(defun disarm-hook-fn ()"
		      " (setq disarm-calls (+ disarm-calls 1))"
		      " (car 1))"));
	CHECK(eval_ok("(add-hook 'before-save-hook 'disarm-hook-fn)"));
	for (i = 0; i < 3; i++) {
		test_status_message[0] = '\0';
		CHECK(eval_ok("(run-hooks 'before-save-hook)"));
		/* Every run reports, which is the other half of "still
		 * armed": Emacs reports once. */
		CHECK(strstr(test_status_message, "Hook error") != nullptr);
	}
	/* Three calls here; Emacs' equivalent shape answers 1. */
	CHECK(eval_eq("disarm-calls", "3"));
	/* And the entry is genuinely still there, not merely re-added: a
	 * repaired definition runs from the same registration. */
	CHECK(eval_ok("(defun disarm-hook-fn () (setq disarm-calls 'healed))"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("disarm-calls", "healed"));
	CHECK(eval_ok("(remove-hook 'before-save-hook 'disarm-hook-fn)"));

	kg_lisp_shutdown();
}

static void test_hooks(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* The value form: a closure handed over directly.  (A defun'd name
	 * would be void as a value -- its function lives in the function
	 * cell, so a bare my-hook read would be void-variable.) */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(setq my-hook (lambda () (setq hook-ran t)))"));
	CHECK(eval_ok("(add-hook 'before-save-hook my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "t"));

	/* remove-hook */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(remove-hook 'before-save-hook my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "nil"));

	/* The quoted-designator form is the Emacs idiom: the symbol is
	 * stored and resolved through its function cell when the hook runs.
	 * It used to register without error and then do nothing, because
	 * the symbol reached FeCallWithOptions unresolved. */
	CHECK(eval_ok("(defun my-hook () (setq hook-ran t))"));
	CHECK(eval_ok("(add-hook 'before-save-hook 'my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "t"));

	/* Resolution happens when the hook runs, not when it is added, so
	 * redefining the function afterwards takes effect -- as in Emacs. */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(defun my-hook () (setq hook-ran 'redefined))"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "redefined"));

	/* ... and removing by the same designator really removes it: the
	 * proof is that the next run leaves the flag alone. */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(remove-hook 'before-save-hook 'my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "nil"));

	/* A symbol whose function cell is empty is a contained hook error
	 * that names the symbol -- what README.md and doc/lisp-api.md
	 * promise.  Resolving the designator before FeCallWithOptions sees
	 * it used to turn this into Fe's anonymous "tried to call
	 * non-callable value". */
	CHECK(eval_ok("(add-hook 'before-save-hook 'never-defined-hook-fn)"));
	test_status_message[0] = '\0';
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(strstr(test_status_message, "Hook error") != nullptr);
	CHECK(strstr(test_status_message, "void-function") != nullptr);
	CHECK(strstr(test_status_message, "never-defined-hook-fn") != nullptr);
	/* Contained: the interpreter is still usable, and defining the name
	 * afterwards makes the very same hook entry work, since resolution
	 * happens when the hook runs. */
	CHECK(eval_ok("(+ 1 2)"));
	CHECK(
	    eval_ok("(defun never-defined-hook-fn () (setq hook-ran 'late))"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "late"));
	CHECK(
	    eval_ok("(remove-hook 'before-save-hook 'never-defined-hook-fn)"));

	/* A cell holding something Fe will not call from the host side -- a
	 * macro, or a primitive -- is named too, and is equally contained.
	 * Both diagnostics used to reach FeCall's own guard, which raises
	 * from a point the guarded frame above does not catch when the hook
	 * runs from a (run-hooks) inside a live evaluator run: the editor
	 * died there rather than printing anything. */
	CHECK(eval_ok("(add-hook 'before-save-hook 'cond)"));
	test_status_message[0] = '\0';
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(strstr(test_status_message, "invalid-function cond") != nullptr);
	CHECK(eval_ok("(remove-hook 'before-save-hook 'cond)"));
	CHECK(eval_ok("(+ 1 2)"));

	/* A cyclic alias chain was the last uncontained one: resolving the
	 * designator raised `cyclic-function-indirection' from a C frame
	 * that had already returned by the time the handler longjmped, so
	 * this case segfaulted rather than reporting anything.  fe's
	 * host-facing resolver answers nil for a cycle now, which lands the
	 * hook in the same reported-not-raised path an empty cell takes.
	 * `void-function' is the condition it lands under, deliberately: a
	 * cycle and a dead multi-link chain are both nil from the resolver
	 * and nothing available here separates them (see
	 * lisp_callable_designator). */
	CHECK(eval_ok("(fset 'cyc-hook 'cyc-hook)"));
	CHECK(eval_ok("(add-hook 'before-save-hook 'cyc-hook)"));
	test_status_message[0] = '\0';
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(strstr(test_status_message, "Hook error") != nullptr);
	CHECK(strstr(test_status_message, "void-function cyc-hook") != nullptr);
	CHECK(eval_ok("(remove-hook 'before-save-hook 'cyc-hook)"));
	CHECK(eval_ok("(+ 1 2)"));
	/* Calling the same name still raises, and is caught as an error
	 * rather than taking the editor down: only the *host*-side resolver
	 * stopped raising. */
	CHECK(eval_error_contains(
	    "(funcall 'cyc-hook)", "cyclic-function-indirection"));
	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
	teardown_editor();
}

/* A process filter named by a quoted symbol must resolve through its
 * function cell when the callback runs.  set-process-filter always
 * accepted symbols, but nothing ever resolved them, so the callback
 * silently never ran; the function-cell-only rule is the same one hooks
 * and functionp follow.  The value namespace is not consulted. */
static void test_process_callback_designator(void)
{
	int i;

	setup_editor();
	kg_process_table_init();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq filter-ran nil)"));
	CHECK(
	    eval_ok("(defalias 'p-filter (lambda (p s) (setq filter-ran t)))"));
	CHECK(eval_ok(
	    "(setq proc (start-shell-command \"filt\" nil \"echo hi\"))"));
	CHECK(eval_ok("(set-process-filter proc 'p-filter)"));

	/* Drive the child to completion the way the editor's main loop
	 * would: poll the process table and drain the published events. */
	for (i = 0; i < 2000 && !eval_eq("filter-ran", "t"); i++) {
		kg_process_table_poll();
		kg_event_drain_safe();
		usleep(1000);
	}
	CHECK(eval_eq("filter-ran", "t"));

	/* A name bound only as a value has an empty function cell, so the
	 * callback never runs -- and says so by name, as a contained
	 * "Process filter error: void-function only-value" on the status
	 * line rather than Fe's anonymous non-callable complaint. */
	CHECK(eval_ok("(setq filter-ran nil)"));
	test_status_message[0] = '\0';
	CHECK(
	    eval_ok("(setq only-value (lambda (p s) (setq filter-ran 'ran)))"));
	CHECK(eval_ok(
	    "(setq proc2 (start-shell-command \"filt2\" nil \"echo yo\"))"));
	CHECK(eval_ok("(set-process-filter proc2 'only-value)"));

	for (i = 0; i < 2000; i++) {
		kg_process_table_poll();
		kg_event_drain_safe();
		usleep(1000);
		if (eval_eq("filter-ran", "t")) {
			break;
		}
	}
	CHECK(eval_eq("filter-ran", "nil"));
	CHECK(strstr(test_status_message, "Process filter error") != nullptr);
	CHECK(strstr(test_status_message, "void-function") != nullptr);
	CHECK(strstr(test_status_message, "only-value") != nullptr);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_keymap_apis(void)
{
	struct keymap *global, *mode;

	setup_editor();
	/* The editor builds these in kbd.c, which this harness does not
	 * link; without them every map name would be one define-key
	 * invented on the spot, and the Emacs spellings below would have
	 * nothing real to resolve to. */
	keymap_reset();
	global = keymap_create("global", KEYMAP_LAYER_GLOBAL);
	mode = keymap_create("dired", KEYMAP_LAYER_MAJOR);
	CHECK(global != nullptr && mode != nullptr);
	keymap_set_active(mode, 0);
	CHECK(kg_lisp_init() == 0);

	/* The Emacs spelling "global-map" resolves to kg's "global". */
	CHECK(eval_ok("(define-key 'global-map \"C-c k\" 'goto-line)"));
	CHECK(eval_eq("(lookup-key 'global-map \"C-c k\")", "goto-line"));
	CHECK(eval_eq("(lookup-key 'global \"C-c k\")", "goto-line"));
	CHECK(eval_eq("(lookup-key 'global-map \"C-c q\")", "nil"));

	/* lookup-key reports what one map says, not what the editor would
	 * do now: an inactive mode map still answers for its own keys, and
	 * does not answer for the global map's. */
	CHECK(eval_ok("(define-key 'dired-mode-map \"C-c d\" 'goto-line)"));
	CHECK(eval_eq("(lookup-key 'dired-mode-map \"C-c d\")", "goto-line"));
	CHECK(eval_eq("(lookup-key 'dired-mode-map \"C-c k\")", "nil"));
	CHECK(eval_eq("(lookup-key 'global-map \"C-c d\")", "nil"));

	/* An unknown map name is nil rather than a map invented for it. */
	CHECK(eval_eq("(lookup-key 'no-such-map \"C-c k\")", "nil"));

	/* current-local-map names the active major-mode map, and nothing
	 * when none is active. */
	CHECK(eval_eq("(current-local-map)", "nil"));
	keymap_set_active(mode, 1);
	CHECK(eval_eq("(current-local-map)", "dired"));

	/* define-key on a name no map answers to creates a major-mode map
	 * rather than failing, and nil as the command unbinds. */
	CHECK(eval_ok("(define-key 'fresh-mode-map \"C-c f\" 'goto-line)"));
	CHECK(eval_eq("(lookup-key 'fresh-mode-map \"C-c f\")", "goto-line"));
	CHECK(eval_ok("(define-key 'fresh-mode-map \"C-c f\" nil)"));
	CHECK(eval_eq("(lookup-key 'fresh-mode-map \"C-c f\")", "nil"));

	/* A sequence keymap_parse_sequence() cannot make sense of is
	 * refused outright rather than half-registered. */
	CHECK(eval_error_contains(
	    "(define-key 'global-map \"not-a-key\" 'goto-line)",
	    "cannot bind key sequence"));

	/* An empty name is not a map, a key or a command. */
	CHECK(eval_error_contains(
	    "(define-key \"\" \"C-c f\" 'goto-line)", "invalid command name"));

	kg_lisp_shutdown();
	teardown_editor();
}

int main(void)
{
	/* Pure and Fe-free, so it is the same function and the same answers
	 * in a WITH_LISP=0 build: run it before the early return. */
	RUN(test_number_token_classifier);
	if (!kg_lisp_active()) {
		RUN(test_disabled);
		return test_summary();
	}
	test_lisp_command_exists = kg_lisp_command_exists;
	test_lisp_command_run = kg_lisp_run_command;

	RUN(test_lifecycle);
	RUN(test_eval_and_recovery);
	RUN(test_sized_input);
	RUN(test_load_file);
	RUN(test_load_file_error);
	RUN(test_load_error_condition_reachability);
	RUN(test_interrupt);
	RUN(test_message_arity);
	RUN(test_insert_and_undo);
	RUN(test_insert_read_only_recovery);
	RUN(test_goto_line_and_buffer_name);
	RUN(test_command_allow_list);
	RUN(test_init_file);
	RUN(test_load_package);
	RUN(test_require_provide);
	RUN(test_require_el_suffix);
	RUN(test_load_path_order);
	RUN(test_load_path_missing_dirs);
	RUN(test_define_and_run_command);
	RUN(test_defun_redefinition_is_atomic);
	RUN(test_interactive_prompt_context_refusal);
	RUN(test_run_command_interrupt);
	RUN(test_run_command_reentrancy);
	RUN(test_command_prefix_delivery);
	RUN(test_key_bindings);
	RUN(test_math_natives);
	RUN(test_point_offsets);
	RUN(test_current_column_tab);
	RUN(test_char_after);
	RUN(test_buffer_substring);
	RUN(test_mark_and_region);
	RUN(test_word_motion);
	RUN(test_word_motion_crosses_rows);
	RUN(test_editor_bridge);
	RUN(test_buffer_objects);
	RUN(test_kill_buffer);
	RUN(test_buffer_stale_slot);
	RUN(test_buffer_point_sync);
	RUN(test_numeric_argument_seam);
	RUN(test_point_survives_set_buffer);
	RUN(test_hidden_buffer_point_persists_across_frames);
	RUN(test_frame_entry_overwrites_from_window_cursor);
	RUN(test_buffer_object_capacity);
	RUN(test_delete_region);
	RUN(test_delete_region_utf8);
	RUN(test_delete_region_malformed_byte);
	RUN(test_delete_region_read_only_recovery);
	RUN(test_replace_region);
	RUN(test_replace_region_read_only_recovery);
	RUN(test_replace_region_alloc_failure_is_atomic);
	RUN(test_search_forward_backward);
	RUN(test_search_forward_bound);
	RUN(test_re_search_and_match_data);
	RUN(test_re_search_backward);
	RUN(test_regex_too_complex_and_bad_pattern);
	RUN(test_search_cancellation);
	RUN(test_markers);
	RUN(test_marker_survives_buffer_kill);
	RUN(test_save_excursion);
	RUN(test_save_excursion_pool_bound);
	RUN(test_with_current_buffer);
	RUN(test_hooks);
	RUN(test_hook_error_does_not_disarm);
	RUN(test_process_callback_designator);
	RUN(test_keymap_apis);
	RUN(test_string_length_and_substring);
	RUN(test_string_concat_and_equal);
	RUN(test_format_natives);
	RUN(test_char_string_round_trip);
	RUN(test_thing_at_point);
	RUN(test_prelude_forms);
	RUN(test_elisp_if);
	RUN(test_list_library);
	RUN(test_type_predicates);
	RUN(test_numeric_core_error_rules);
	RUN(test_binding_forms);
	RUN(test_phase11_dynamic_binding);
	RUN(test_definition_forms);
	RUN(test_strict_arity);
	RUN(test_writer_quote_abbreviation);
	RUN(test_quasiquote);
	RUN(test_void_function);
	RUN(test_void_variable);
	RUN(test_cyclic_result);
	RUN(test_recursion_depth);
	RUN(test_arena_exhaustion_conditions);
	RUN(test_condition_case_native_error);
	RUN(test_catch_throw_unwind);
	RUN(test_wrapping_native_transparency);
	RUN(test_condition_case_kg_native_conditions);
	RUN(test_hook_quit_is_not_contained);
	RUN(test_hook_throw_containment);
	RUN(test_signal);
	RUN(test_ignore_errors);
	RUN(test_condition_case_reentry);
	RUN(test_cleanup_containment_keeps_condition);
	RUN(test_quit_uncaught);
	RUN(test_phase8_constants_and_keywords);
	RUN(test_phase8_reader_literals);
	RUN(test_phase8_library);
	RUN(test_prelude_source_file);
	return test_summary();
}
