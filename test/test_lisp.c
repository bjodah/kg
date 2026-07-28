/* test_lisp.c - Fe interpreter lifecycle regression tests */

#include "../src/def.h"
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

/* word.o is linked for forward-word/backward-word; its interactive
 * zap-to-char primitive drags in terminal entry points the Lisp tests
 * never reach. */
void editor_refresh_screen(void) { }

int editor_read_raw_byte(int fd)
{
	(void)fd;
	return 0;
}

static void setup_editor(void)
{
	free_all_rows();
	undo_free();
	memset(&editor, 0, sizeof(editor));
	editor.screenrows = 24;
	editor.screencols = 80;
	editor.filename = "/tmp/bridge.txt";
	suppress_undo = 0;
	undo_init();
	test_status_message[0] = '\0';
	test_command_name[0] = '\0';
	test_command_calls = 0;
}

static void teardown_editor(void)
{
	free_all_rows();
	editor.row = nullptr;
	editor.numrows = 0;
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
	static const char source[] = "(= loaded-value 41)\n"
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
	CHECK(kg_lisp_eval_string(
		  "(message \"a\" \"b\")", 17, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "too many arguments") != nullptr);
	CHECK(kg_lisp_eval_string("(message 1)", 11, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "expected string") != nullptr);
	CHECK(kg_lisp_eval_string(
		  "(message \"ready\")", 17, result, sizeof(result))
	    == 0);
	CHECK(strcmp(test_status_message, "ready") == 0);
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
	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == (int)payload_len);
	CHECK(memcmp(
		  editor.row[0].chars, source + sizeof(prefix) - 1, payload_len)
	    == 0);
	CHECK(undostack.size == 1);
	editor_undo();
	CHECK(editor.row[0].size == 0);
	kg_lisp_shutdown();
	free(source);
	teardown_editor();
}

static void test_insert_read_only_recovery(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(0, "original", 8);
	editor.readonly = 1;
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string(
		  "(insert \"changed\")", 18, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "buffer is read-only") != nullptr);
	CHECK(editor.row[0].size == 8);
	CHECK(memcmp(editor.row[0].chars, "original", 8) == 0);
	CHECK(kg_lisp_eval_string("(+ 5 6)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "11") == 0);
	kg_lisp_shutdown();
	teardown_editor();
}

static void test_goto_line_and_buffer_name(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(0, "zero", 4);
	editor_insert_row(1, "second", 6);
	editor_insert_row(2, "third", 5);
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
	CHECK(write_text_file(path, "(= init-loaded 42)\n") == 0);
	CHECK(kg_lisp_load_init() == 0);
	CHECK(kg_lisp_eval_string("init-loaded", 11, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "42") == 0);

	/* A broken init file reports its labelled error; forms evaluated
	 * before the failure remain applied. */
	CHECK(write_text_file(path, "(= init-partial 1)\n(car 1)\n") == 0);
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
	CHECK(write_text_file(path, "(= pkg-value 7)\n") == 0);
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
	CHECK(write_text_file(path, "(load \"pkg-b\")\n(= a-after b-value)\n")
	    == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-b.fe", root);
	CHECK(write_text_file(path, "(= b-value 5)\n") == 0);
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
	CHECK(write_text_file(path, "(= direct-value 3)\n") == 0);
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
	CHECK(
	    eval_ok("(define-command \"greet\" (fn () (message \"hello\")))"));
	CHECK(kg_lisp_command_name(0) != nullptr);
	CHECK(strcmp(kg_lisp_command_name(0), "greet") == 0);
	CHECK(kg_lisp_command_name(1) == nullptr);
	CHECK(kg_lisp_run_command("greet", 0) == 0);
	CHECK(strcmp(test_status_message, "hello") == 0);

	/* Redefinition replaces the function (and releases the old root). */
	CHECK(
	    eval_ok("(define-command \"greet\" (fn () (message \"again\")))"));
	CHECK(kg_lisp_run_command("greet", 0) == 0);
	CHECK(strcmp(test_status_message, "again") == 0);
	CHECK(kg_lisp_command_name(1) == nullptr);

	/* Errors inside a command return to the caller with a message. */
	CHECK(eval_ok("(define-command \"boom\" (fn () (car 1)))"));
	CHECK(kg_lisp_run_command("boom", 0) == 0);
	CHECK(strstr(test_status_message, "Lisp error") != nullptr);
	CHECK(kg_lisp_run_command("greet", 0) == 0);
	CHECK(strcmp(test_status_message, "again") == 0);

	/* A runaway command hits the step budget and recovers. */
	CHECK(eval_ok("(define-command \"spin\" (fn () (while t 1)))"));
	CHECK(kg_lisp_run_command("spin", 0) == 0);
	CHECK(strstr(test_status_message, "step limit") != nullptr);
	CHECK(eval_ok("(+ 1 2)"));

	CHECK(eval_ok("(remove-command \"boom\")"));
	CHECK(kg_lisp_run_command("boom", 0) != 0);
	CHECK(eval_error_contains(
	    "(remove-command \"boom\")", "no such Lisp command"));

	CHECK(eval_error_contains(
	    "(define-command \"version\" (fn () 1))", "built-in"));
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

	CHECK(eval_ok("(define-command \"greet\" (fn () (message \"hey\")))"));
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
	editor_insert_row(0, "abc", 3);
	editor_insert_row(1, "h\xc3\xa9llo", 6);
	editor_insert_row(2, "\xe6\xbc\xa2\xe5\xad\x97", 6);
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
	CHECK(eval_eq("(do (goto-char -400) (point))", "1"));
	CHECK(eval_eq("(do (goto-char 0) (point))", "1"));
	CHECK(eval_eq("(do (goto-char 9999) (point))", "13"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_current_column_tab(void)
{
	setup_editor();
	editor_insert_row(0, "\tx", 2);
	CHECK(kg_lisp_init() == 0);

	/* Display column, so the tab expands to the next tab stop (8),
	 * matching Emacs' current-column. */
	CHECK(eval_eq("(do (goto-char 1) (current-column))", "0"));
	CHECK(eval_eq("(do (goto-char 2) (current-column))", "8"));
	CHECK(eval_eq("(do (goto-char 3) (current-column))", "9"));

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
	CHECK(eval_eq("(do (goto-char 6) (char-after))", "233"));
	CHECK(eval_eq("(do (goto-char 6) (char-after nil))", "233"));
	CHECK(eval_eq("(do (goto-char (point-max)) (char-after))", "nil"));

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
	dirty_before = editor.dirty;

	/* No mark: (mark) is nil and the region accessors signal. */
	CHECK(eval_eq("(mark)", "nil"));
	CHECK(eval_error_contains("(region-beginning)", "no region"));
	CHECK(eval_error_contains("(region-end)", "no region"));

	CHECK(eval_ok("(set-mark 5)"));
	CHECK(editor.mark_set == 1);
	CHECK(editor.mark_highlight == 1);
	CHECK(eval_eq("(mark)", "5"));
	CHECK(eval_eq("(do (goto-char 9) (region-beginning))", "5"));
	CHECK(eval_eq("(region-end)", "9"));
	CHECK(eval_eq("(buffer-substring (region-beginning) (region-end))",
	    "h\xc3\xa9ll"));

	/* Mark after point still yields an ordered region. */
	CHECK(eval_ok("(do (set-mark 9) (goto-char 5))"));
	CHECK(eval_eq("(region-beginning)", "5"));
	CHECK(eval_eq("(region-end)", "9"));

	/* Out-of-range marks clamp. */
	CHECK(eval_ok("(set-mark 9999)"));
	CHECK(eval_eq("(mark)", "13"));

	/* deactivate-mark drops the highlight but keeps the mark. */
	CHECK(eval_ok("(deactivate-mark)"));
	CHECK(editor.mark_highlight == 0);
	CHECK(editor.mark_set == 1);
	CHECK(eval_eq("(mark)", "13"));

	/* None of this touches the buffer. */
	CHECK(editor.dirty == dirty_before);

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_word_motion(void)
{
	setup_editor();
	editor_insert_row(0, "one two three", 13);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_eq("(do (goto-char 1) (forward-word) (point))", "4"));
	CHECK(eval_eq("(do (goto-char 1) (forward-word 2) (point))", "8"));
	/* A huge count stops at the edge instead of spinning. */
	CHECK(eval_eq("(do (goto-char 1) (forward-word 9999) (point))", "14"));
	CHECK(eval_eq(
	    "(do (goto-char (point-max)) (backward-word) (point))", "9"));
	CHECK(eval_eq(
	    "(do (goto-char (point-max)) (backward-word 9999) (point))", "1"));
	/* Negative counts reverse direction, as in Emacs. */
	CHECK(eval_eq("(do (goto-char 1) (backward-word -1) (point))", "4"));
	CHECK(eval_eq(
	    "(do (goto-char (point-max)) (forward-word -1) (point))", "9"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_editor_bridge(void)
{
	setup_editor();
	editor_insert_row(0, "abc", 3);
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(message \"bridged\")"));
	CHECK(strcmp(test_status_message, "bridged") == 0);
	CHECK(eval_eq("(buffer-name)", "bridge.txt"));
	CHECK(eval_ok("(do (goto-char 1) (insert \"z\"))"));
	CHECK(eval_eq("(buffer-substring (point-min) (point-max))", "zabc"));
	CHECK(eval_ok("(define-command \"greet\" (fn () (message \"hey\")))"));
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
	editor_insert_row(0, "one  two", 8);
	editor_insert_row(1, "", 0);
	editor_insert_row(2, "h\xc3\xa9llo w\xc3\xb6rld", 13);
	editor_insert_row(3, "\xe6\xbc\xa2\xe5\xad\x97", 6);
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
	CHECK(eval_eq(
	    "(do (goto-char 1) (bounds-of-thing-at-point 'word))", "(1 . 4)"));
	CHECK(eval_eq(
	    "(do (goto-char 2) (bounds-of-thing-at-point 'word))", "(1 . 4)"));
	CHECK(eval_eq(
	    "(do (goto-char 4) (bounds-of-thing-at-point 'word))", "(1 . 4)"));
	CHECK(eval_eq(
	    "(do (goto-char 6) (bounds-of-thing-at-point 'word))", "(6 . 9)"));
	CHECK(eval_eq(
	    "(do (goto-char 9) (bounds-of-thing-at-point 'word))", "(6 . 9)"));
	/* Between two words, and on an empty line: no word. */
	CHECK(eval_eq(
	    "(do (goto-char 5) (bounds-of-thing-at-point 'word))", "nil"));
	CHECK(eval_eq(
	    "(do (goto-char 10) (bounds-of-thing-at-point 'word))", "nil"));

	/* Codepoints at or above U+0080 are word constituents here, unlike
	 * kg's ASCII-only interactive word motion. */
	CHECK(eval_eq("(do (goto-char 11) (bounds-of-thing-at-point 'word))",
	    "(11 . 16)"));
	CHECK(eval_eq("(do (goto-char 12) (bounds-of-thing-at-point 'word))",
	    "(11 . 16)"));
	CHECK(eval_eq("(do (goto-char 18) (bounds-of-thing-at-point 'word))",
	    "(17 . 22)"));
	CHECK(eval_eq("(do (goto-char 23) (bounds-of-thing-at-point 'word))",
	    "(23 . 25)"));
	CHECK(eval_eq("(do (goto-char 24) (bounds-of-thing-at-point 'word))",
	    "(23 . 25)"));

	/* 'line takes in its line break, as in Emacs, so END is the start of
	 * the next row.  emacs -Q --batch on the same text returns (1 . 10),
	 * (10 . 11), (11 . 23) and (23 . 25). */
	CHECK(eval_eq(
	    "(do (goto-char 5) (bounds-of-thing-at-point 'line))", "(1 . 10)"));
	CHECK(eval_eq("(do (goto-char 10) (bounds-of-thing-at-point 'line))",
	    "(10 . 11)"));
	CHECK(eval_eq("(do (goto-char 15) (bounds-of-thing-at-point 'line))",
	    "(11 . 23)"));
	/* The last row has no next row, so its bounds stop at point-max. */
	CHECK(eval_eq("(do (goto-char 23) (bounds-of-thing-at-point 'line))",
	    "(23 . 25)"));
	CHECK(eval_eq("(do (goto-char 25) (bounds-of-thing-at-point 'line))",
	    "(23 . 25)"));

	/* car/cdr work on the result, as with any Emacs bounds pair. */
	CHECK(eval_eq(
	    "(car (do (goto-char 12) (bounds-of-thing-at-point 'word)))",
	    "11"));
	CHECK(eval_eq(
	    "(cdr (do (goto-char 12) (bounds-of-thing-at-point 'word)))",
	    "16"));

	/* The prelude's thing-at-point is the text of those bounds. */
	CHECK(eval_eq(
	    "(do (goto-char 12) (thing-at-point 'word))", "h\xc3\xa9llo"));
	CHECK(eval_eq("(do (goto-char 23) (thing-at-point 'word))",
	    "\xe6\xbc\xa2\xe5\xad\x97"));
	CHECK(eval_eq("(do (goto-char 5) (thing-at-point 'word))", "nil"));
	/* The line break is part of the text, so it is part of the string. */
	CHECK(
	    eval_eq("(do (goto-char 5) (thing-at-point 'line))", "one  two\n"));
	CHECK(eval_eq("(do (goto-char 10) (thing-at-point 'line))", "\n"));
	CHECK(eval_eq("(do (goto-char 23) (thing-at-point 'line))",
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
	CHECK(eval_eq(
	    "(do (= seen 0) (cond (t (= seen 1) 2) (t (= seen 9))) seen)",
	    "1"));

	CHECK(eval_eq("(when t 1 2)", "2"));
	CHECK(eval_eq("(when nil 1)", "nil"));
	CHECK(eval_eq("(do (= n 0) (when t (= n 5)) n)", "5"));
	CHECK(eval_eq("(do (= n 0) (when nil (= n 5)) n)", "0"));
	CHECK(eval_eq("(unless nil 1 2)", "2"));
	CHECK(eval_eq("(unless t 1)", "nil"));
	CHECK(eval_eq("(do (= n 0) (unless nil (= n 7)) n)", "7"));
	CHECK(eval_eq("(do (= n 0) (unless t (= n 7)) n)", "0"));

	/* dolist accumulates and leaves the loop variable local. */
	CHECK(eval_eq("(do (= acc \"\") (dolist (x (list \"a\" \"b\" \"c\"))"
		      " (= acc (concat acc x))) acc)",
	    "abc"));
	CHECK(eval_eq(
	    "(do (= n 0) (dolist (x (list 1 2 3)) (= n (+ n x))) n)", "6"));
	CHECK(eval_eq("(do (= n 0) (dolist (x nil) (= n 1)) n)", "0"));
	/* Nested loops do not capture each other's variable. */
	CHECK(eval_eq("(do (= acc \"\") (dolist (a (list \"1\" \"2\"))"
		      " (dolist (b (list \"x\" \"y\"))"
		      "  (= acc (concat acc a b)))) acc)",
	    "1x1y2x2y"));

	CHECK(eval_eq("(string-empty-p \"\")", "t"));
	CHECK(eval_eq("(string-empty-p \"a\")", "nil"));
	CHECK(eval_eq("(string-empty-p (substring \"abc\" 1 1))", "t"));

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
	RUN(test_char_string_round_trip);
	RUN(test_thing_at_point);
	RUN(test_prelude_forms);
	return test_summary();
}
