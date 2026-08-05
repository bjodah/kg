/* test_lisp.c - Fe interpreter lifecycle regression tests */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/keybind.h"
#include "../src/keymap.h"
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

	/* Missing packages raise an error naming the resolved path. */
	CHECK(
	    kg_lisp_eval_string("(load \"absent\")", 15, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "absent.el") != nullptr);
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
	CHECK(strstr(test_status_message, "Lisp error") != nullptr);
	CHECK(strstr(test_status_message, "evaluation cancelled") != nullptr);
	CHECK(strstr(test_status_message, "spin") != nullptr);
	kg_lisp_set_interrupt_check(nullptr);

	/* A cancelled run leaves the interpreter usable, and without the
	 * interrupt the same command now exhausts the step budget. */
	CHECK(kg_lisp_run_command("spin", 0) == 0);
	CHECK(strstr(test_status_message, "step limit") != nullptr);
	CHECK(eval_ok("(+ 1 1)"));

	kg_lisp_shutdown();
	teardown_editor();
}

static void test_run_command_reentrancy(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	/* A Lisp-defined command cannot re-enter another from Lisp origin:
	 * lisp_defined_command is not CMD_LISP_CALLABLE, so command-execute
	 * refuses it before any nested run.  There is no callback queue. */
	CHECK(eval_ok("(define-command \"inner\""
		      " (lambda () (message \"inner-ran\")))"));
	CHECK(eval_ok("(define-command \"outer\""
		      " (lambda () (command-execute 'inner)))"));
	CHECK(kg_lisp_run_command("outer", 0) == 0);
	CHECK(strstr(test_status_message, "Lisp error") != nullptr);
	CHECK(strstr(test_status_message, "outer") != nullptr);
	CHECK(strstr(test_status_message, "command is not allowed") != nullptr);
	CHECK(strstr(test_status_message, "inner") != nullptr);
	/* The nested command never ran, and an independent later invocation
	 * works normally. */
	CHECK(kg_lisp_run_command("inner", 0) == 0);
	CHECK(strcmp(test_status_message, "inner-ran") == 0);

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
	CHECK(strstr(result, "evaluation cancelled") != nullptr);
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
 * 1100, and `(deep 200)` alone reaches peak_frame_depth 604 -- about 3.02
 * frames per recursion level for this chain's shape (`if`, `+`, and the
 * recursive call each open a frame). `(deep 1000000)` therefore asks for
 * roughly 3 million frames against a 1100-frame arena, more than 2700x
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

/* Sub-plan 01A's second test path: lisp/prelude.el is the canonical source
 * and src/lisp_prelude_generated.inc is a generated copy of it that kg
 * evaluates at startup.  `make lisp-prelude-check` proves those two files
 * agree byte for byte; this proves the *file* and the *running editor*
 * agree definition for definition, and that the definitions are in the
 * order the prelude's first rule requires.
 *
 * It deliberately does not re-load the file into a booted context.  A
 * second evaluation is not idempotent, for exactly the reason rule 1
 * exists: `(setq internal--let let)` aliases Fe's raw `let` primitive
 * before the Emacs `let` macro shadows that name, so re-running it where
 * the macro already exists would bind internal--let to the macro and
 * break every list-library function that uses it.  That hazard is what
 * the type assertions below detect -- internal--let must answer
 * `primitive`, not `macro`, so a generator that ever reordered or dropped
 * forms fails here rather than silently shipping a broken prelude.
 *
 * Arity is not asserted separately: kg's Lisp surface has no func-arity
 * and a lambda's printed form carries no parameter list, so each
 * definition's type plus the byte-identity check above is what is
 * actually observable.
 *
 * Sub-plan 02D's dialect cutover deleted the kg-owned `setq` macro (built
 * on assignment `=`) and rewrote the remaining 53 definitions from `=' to
 * core `setq'; the scan below looks for column-zero "(setq NAME " forms
 * accordingly. */
#define PRELUDE_DEFS 53

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

	/* A definition is "(setq NAME " in column 0; every continuation line
	 * in the file is indented, so nothing nested is ever found here. */
	for (line = text; line != nullptr; line = strchr(line, '\n')) {
		const char *name_start, *value_start;
		size_t name_len;

		if (line[0] == '\n') {
			line++;
		}
		if (strncmp(line, "(setq ", 6) != 0) {
			continue;
		}
		CHECK(count < PRELUDE_DEFS);
		if (count >= PRELUDE_DEFS) {
			break;
		}
		name_start = line + 6;
		name_len = strcspn(name_start, " )\n");
		CHECK(name_len > 0 && name_len < sizeof(names[0]));
		memcpy(names[count], name_start, name_len);
		names[count][name_len] = '\0';

		/* The declared shape decides the type the editor must report:
		 * a lambda form is a lambda, a macro form is a macro, and the
		 * four bare-symbol forms are aliases of Fe primitives. */
		value_start = name_start + name_len;
		value_start += strspn(value_start, " ");
		if (strncmp(value_start, "(lambda", 7) == 0) {
			strcpy(types[count], "lambda");
		} else if (strncmp(value_start, "(macro", 6) == 0) {
			strcpy(types[count], "macro");
		} else {
			strcpy(types[count], "primitive");
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

		(void)snprintf(
		    source, sizeof(source), "(type-of %s)", names[i]);
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

static void test_hooks(void)
{
	setup_editor();
	CHECK(kg_lisp_init() == 0);

	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(defun my-hook () (setq hook-ran t))"));
	CHECK(eval_ok("(add-hook 'before-save-hook my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "t"));

	/* remove-hook */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(remove-hook 'before-save-hook my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "nil"));

	/* A quoted symbol is the Emacs idiom and must work.  It used to
	 * register without error and then do nothing, because the symbol
	 * reached FeCallWithOptions unresolved. */
	CHECK(eval_ok("(add-hook 'before-save-hook 'my-hook)"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "t"));

	/* Resolution happens when the hook runs, not when it is added, so
	 * redefining the function afterwards takes effect -- as in Emacs. */
	CHECK(eval_ok("(setq hook-ran nil)"));
	CHECK(eval_ok("(defun my-hook () (setq hook-ran 'redefined))"));
	CHECK(eval_ok("(run-hooks 'before-save-hook)"));
	CHECK(eval_eq("hook-ran", "redefined"));

	CHECK(eval_ok("(remove-hook 'before-save-hook 'my-hook)"));

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
	RUN(test_require_provide);
	RUN(test_load_path_order);
	RUN(test_load_path_missing_dirs);
	RUN(test_define_and_run_command);
	RUN(test_run_command_interrupt);
	RUN(test_run_command_reentrancy);
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
	RUN(test_with_current_buffer);
	RUN(test_hooks);
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
	RUN(test_binding_forms);
	RUN(test_definition_forms);
	RUN(test_quasiquote);
	RUN(test_void_function);
	RUN(test_void_variable);
	RUN(test_cyclic_result);
	RUN(test_recursion_depth);
	RUN(test_prelude_source_file);
	return test_summary();
}
