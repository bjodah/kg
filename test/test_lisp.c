/* test_lisp.c - Fe interpreter lifecycle regression tests */

#include "../src/def.h"
#include "../src/keybind.h"
#include "../src/lisp.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char test_status_message[512];
extern char test_command_name[128];
extern int test_command_calls;

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
	bcur()->filename = "/tmp/bridge.txt";
	suppress_undo = 0;
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
	CHECK(kg_lisp_load_file("unused.fe") != 0);
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
	CHECK(kg_lisp_load_file("/tmp/kg-lisp-missing/no-file.fe") != 0);
	CHECK(strstr(kg_lisp_last_error(), "cannot open") != nullptr);
	CHECK(strstr(kg_lisp_last_error(), "no-file.fe") != nullptr);
	kg_lisp_shutdown();
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
	CHECK(strstr(result, "evaluation cancelled") != nullptr);
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
		"%s/kg/lisp/pkg.fe",
		"%s/kg/lisp/pkg-a.fe",
		"%s/kg/lisp/pkg-b.fe",
		"%s/kg/lisp/pkg-x.fe",
		"%s/direct.fe",
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

	(void)snprintf(path, sizeof(path), "%s/kg/init.fe", root);
	CHECK(write_text_file(path, "(setq init-loaded 42)\n") == 0);
	CHECK(kg_lisp_load_init() == 0);
	CHECK(kg_lisp_eval_string("init-loaded", 11, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "42") == 0);

	/* A broken init file reports its labelled error; forms evaluated
	 * before the failure remain applied. */
	CHECK(write_text_file(path, "(setq init-partial 1)\n(car 1)\n") == 0);
	CHECK(kg_lisp_load_init() != 0);
	CHECK(strstr(kg_lisp_last_error(), "init.fe") != nullptr);
	CHECK(kg_lisp_eval_string("init-partial", 12, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "1") == 0);

	kg_lisp_shutdown();
	remove_config_root(root);
}

static void test_load_package(void)
{
	char root[64], path[512], source[600], result[128] = "";
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* Bare names resolve to <config>/kg/lisp/NAME.fe. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg.fe", root);
	CHECK(write_text_file(path, "(setq pkg-value 7)\n") == 0);
	CHECK(kg_lisp_eval_string("(load \"pkg\")", 12, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("pkg-value", 9, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "7") == 0);

	/* Missing packages raise an error naming the resolved path. */
	CHECK(
	    kg_lisp_eval_string("(load \"absent\")", 15, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "absent.fe") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 1 1)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "2") == 0);

	/* Packages may load packages. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-a.fe", root);
	CHECK(
	    write_text_file(path, "(load \"pkg-b\")\n(setq a-after b-value)\n")
	    == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-b.fe", root);
	CHECK(write_text_file(path, "(setq b-value 5)\n") == 0);
	CHECK(
	    kg_lisp_eval_string("(load \"pkg-a\")", 14, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("a-after", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "5") == 0);

	/* Load cycles hit the depth limit and recover. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-x.fe", root);
	CHECK(write_text_file(path, "(load \"pkg-x\")\n") == 0);
	CHECK(
	    kg_lisp_eval_string("(load \"pkg-x\")", 14, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "depth limit") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 2 2)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "4") == 0);

	/* Names containing '/' are literal paths. */
	(void)snprintf(path, sizeof(path), "%s/direct.fe", root);
	CHECK(write_text_file(path, "(setq direct-value 3)\n") == 0);
	length = snprintf(source, sizeof(source), "(load \"%s\")", path);
	CHECK(length > 0 && (size_t)length < sizeof(source));
	CHECK(
	    kg_lisp_eval_string(source, (size_t)length, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("direct-value", 12, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "3") == 0);

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
	kg_lisp_shutdown();
	teardown_editor();
}

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

	/* Errors inside a command return to the caller with a message. */
	CHECK(eval_ok("(define-command \"boom\" (lambda () (car 1)))"));
	CHECK(kg_lisp_run_command("boom", 0) == 0);
	CHECK(strstr(test_status_message, "Lisp error") != nullptr);
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

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_key_bindings(void)
{
	int key;

	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(keybind_parse("C-c i", &key) == 0 && key == 'i');
	CHECK(keybind_parse("C-c C-y", &key) == 0 && key == ('y' & 0x1f));
	CHECK(keybind_parse("C-x i", &key) != 0);
	CHECK(keybind_parse("C-c", &key) != 0);
	CHECK(keybind_parse("C-c  i", &key) != 0);
	CHECK(keybind_parse("C-c ii", &key) != 0);
	CHECK(keybind_parse("C-c C-g", &key) != 0);

	CHECK(eval_ok(
	    "(define-command \"greet\" (lambda () (message \"hey\")))"));
	CHECK(eval_ok("(global-set-key \"C-c i\" \"greet\")"));
	CHECK(keybind_lookup('i') != nullptr);
	CHECK(strcmp(keybind_lookup('i'), "greet") == 0);
	CHECK(eval_error_contains(
	    "(global-set-key \"C-x i\" \"greet\")", "invalid key sequence"));
	CHECK(eval_ok("(global-unset-key \"C-c i\")"));
	CHECK(keybind_lookup('i') == nullptr);
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
	CHECK(bcur()->mark_set == 1);
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
	CHECK(bcur()->mark_set == 1);
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
	CHECK(keybind_lookup('i') != nullptr);
	CHECK(eval_ok("(global-unset-key \"C-c i\")"));
	CHECK(keybind_lookup('i') == nullptr);
	CHECK(
	    eval_error_contains("(load \"absent-package\")", "absent-package"));

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
	/* Either side of the int64 fast path, whose guard keeps the cast
	 * defined; the wide values are exact, as they are in Emacs. */
	CHECK(eval_eq("(format \"%d\" 9007199254740993)", "9007199254740992"));
	CHECK(eval_eq("(format \"%d\" 1e19)", "10000000000000000000"));
	CHECK(eval_eq("(format \"%d\" -1e19)", "-10000000000000000000"));
	CHECK(eval_eq("(string-length (format \"%d\" 1e300))", "301"));
	CHECK(eval_eq("(substring (format \"%d\" 1e300) 0 4)", "1000"));
	/* kg has no bignums to print NaN or an infinity into, so unlike
	 * Emacs, which writes "nan" and "inf", %d refuses them. */
	CHECK(eval_error_contains("(format \"%d\" (/ 1 0))", "finite"));
	CHECK(eval_error_contains(
	    "(format \"%d\" (- (/ 1 0) (/ 1 0)))", "finite"));
	/* %s and %S spell these the C way, not as Emacs' readable float
	 * syntax "-0.0e+NaN" and "1.0e+INF".  Deliberate: fe has one number
	 * type, so kg prints 42.0 as "42" already, and float syntax for the
	 * exceptional values alone would be the odd case out. */
	CHECK(eval_eq("(format \"%s\" (/ 1 0))", "inf"));
	CHECK(eval_eq("(format \"%S\" (/ 1 0))", "inf"));
	CHECK(eval_eq("(format \"%s\" (- 0 (/ 1 0)))", "-inf"));

	/* %e, %f and %g are C's conversions, which is exactly what Emacs
	 * uses; every one of these was checked against Emacs 31. */
	CHECK(eval_eq("(format \"%e\" 1.5)", "1.500000e+00"));
	CHECK(eval_eq("(format \"%e\" 42)", "4.200000e+01"));
	CHECK(eval_eq("(format \"%e\" 1e300)", "1.000000e+300"));
	CHECK(eval_eq("(format \"%f\" 1.5)", "1.500000"));
	CHECK(eval_eq("(format \"%f\" -2.25)", "-2.250000"));
	CHECK(eval_eq("(format \"%g\" 1.5)", "1.5"));
	CHECK(eval_eq("(format \"%g\" 0)", "0"));
	CHECK(eval_eq("(format \"%g\" 1e300)", "1e+300"));
	/* Unlike %d, the float conversions have a rendering for the
	 * exceptional values, so they print them instead of raising. */
	CHECK(eval_eq("(format \"%e\" (/ 1 0))", "inf"));
	CHECK(eval_eq("(format \"%f\" (- 0 (/ 1 0)))", "-inf"));
	CHECK(eval_eq("(format \"%g\" (/ 1 0))", "inf"));
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
	/* Recovery after any of those leaves the interpreter usable. */
	CHECK(eval_eq("(format \"%s\" 'ok)", "ok"));

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
	CHECK(eval_eq("(type-of 1)", "double"));
	CHECK(eval_eq("(type-of (cons 1 2))", "pair"));
	CHECK(eval_eq("(type-of nil)", "nil"));
	CHECK(eval_eq("(type-of car)", "primitive"));
	CHECK(eval_eq("(type-of insert)", "native-fn"));
	CHECK(eval_eq("(type-of (lambda (x) x))", "lambda"));
	CHECK(eval_eq("(type-of cond)", "macro"));

	CHECK(eval_eq("(stringp \"a\")", "t"));
	CHECK(eval_eq("(stringp 'a)", "nil"));
	CHECK(eval_eq("(numberp 1)", "t"));
	CHECK(eval_eq("(numberp \"1\")", "nil"));
	CHECK(eval_eq("(consp (cons 1 2))", "t"));
	CHECK(eval_eq("(consp nil)", "nil"));
	/* nil and t are symbols in Emacs, and here too. */
	CHECK(eval_eq("(symbolp 'a)", "t"));
	CHECK(eval_eq("(symbolp nil)", "t"));
	CHECK(eval_eq("(symbolp \"a\")", "nil"));
	CHECK(eval_eq("(functionp (lambda (x) x))", "t"));
	CHECK(eval_eq("(functionp car)", "t"));
	CHECK(eval_eq("(functionp insert)", "t"));
	CHECK(eval_eq("(functionp cond)", "nil"));
	CHECK(eval_eq("(functionp 1)", "nil"));
	CHECK(eval_eq("(listp nil)", "t"));
	CHECK(eval_eq("(listp (cons 1 2))", "t"));
	CHECK(eval_eq("(listp 1)", "nil"));
	CHECK(eval_error_contains("(stringp)", "too few arguments"));
	CHECK(eval_error_contains("(type-of 1 2)", "too many arguments"));

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
	/* Closures capture the let bindings. */
	CHECK(eval_eq("(progn (setq mk (let ((n 10)) (lambda (x) (+ x n))))"
		      " (mk 5))",
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

	kg_lisp_shutdown();
}

static void test_definition_forms(void)
{
	CHECK(kg_lisp_init() == 0);

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
	/* A defun without (interactive) registers nothing. */
	CHECK(eval_ok("(defun quiet-lisp () (message \"no\"))"));
	CHECK(kg_lisp_command_name(1) == nullptr);

	kg_lisp_shutdown();
	teardown_editor();
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
	CHECK(eval_eq("(progn (setq b 42) `,b)", "42"));
	/* An empty backquote is a real nil, not a nil-shaped object. */
	CHECK(eval_eq("`()", "nil"));
	CHECK(eval_eq("(not `())", "t"));
	/* Backticks and commas inside strings stay inert. */
	CHECK(eval_eq("\"a, b `c` ,@d\"", "a, b `c` ,@d"));
	/* #'x is plain x, since Fe has one namespace. */
	CHECK(eval_eq("(mapcar #'car (list (list 1 2) (list 3 4)))", "(1 3)"));
	CHECK(eval_eq("(function car)", "[primitive]"));

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

/* Fe's GC stack is what bounds recursion, not the C stack. */
static void test_recursion_depth(void)
{
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(defun deep (n) (if (<= n 0) 0 (+ 1 (deep (- n 1)))))"));
	CHECK(eval_eq("(deep 200)", "200"));
	CHECK(eval_error_contains("(deep 5000)", "GC stack overflow"));
	CHECK(eval_ok("(+ 1 2)"));

	kg_lisp_shutdown();
}

int main(void)
{
	if (!kg_lisp_active()) {
		RUN(test_disabled);
		return test_summary();
	}

	RUN(test_lifecycle);
	RUN(test_eval_and_recovery);
	RUN(test_sized_input);
	RUN(test_load_file);
	RUN(test_load_file_error);
	RUN(test_interrupt);
	RUN(test_message_arity);
	RUN(test_insert_and_undo);
	RUN(test_insert_read_only_recovery);
	RUN(test_goto_line_and_buffer_name);
	RUN(test_command_allow_list);
	RUN(test_init_file);
	RUN(test_load_package);
	RUN(test_define_and_run_command);
	RUN(test_key_bindings);
	RUN(test_math_natives);
	RUN(test_point_offsets);
	RUN(test_current_column_tab);
	RUN(test_char_after);
	RUN(test_buffer_substring);
	RUN(test_mark_and_region);
	RUN(test_word_motion);
	RUN(test_editor_bridge);
	RUN(test_string_length_and_substring);
	RUN(test_string_concat_and_equal);
	RUN(test_format_natives);
	RUN(test_char_string_round_trip);
	RUN(test_thing_at_point);
	RUN(test_prelude_forms);
	RUN(test_elisp_if);
	RUN(test_list_library);
	RUN(test_type_predicates);
	RUN(test_binding_forms);
	RUN(test_definition_forms);
	RUN(test_quasiquote);
	RUN(test_void_function);
	RUN(test_void_variable);
	RUN(test_cyclic_result);
	RUN(test_recursion_depth);
	return test_summary();
}
