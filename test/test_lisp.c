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
	CHECK(kg_lisp_eval_string("(kg-message)", 12, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "too few arguments") != nullptr);
	CHECK(kg_lisp_eval_string(
		  "(kg-message \"a\" \"b\")", 20, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "too many arguments") != nullptr);
	CHECK(kg_lisp_eval_string("(kg-message 1)", 14, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "expected string") != nullptr);
	CHECK(kg_lisp_eval_string(
		  "(kg-message \"ready\")", 20, result, sizeof(result))
	    == 0);
	CHECK(strcmp(test_status_message, "ready") == 0);
	kg_lisp_shutdown();
	teardown_editor();
}

static void test_insert_and_undo(void)
{
	static constexpr size_t payload_len = 2048;
	static const char prefix[] = "(kg-insert \"";
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
		  "(kg-insert \"changed\")", 21, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "buffer is read-only") != nullptr);
	CHECK(editor.row[0].size == 8);
	CHECK(memcmp(editor.row[0].chars, "original", 8) == 0);
	CHECK(kg_lisp_eval_string("(+ 5 6)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "11") == 0);
	kg_lisp_shutdown();
	teardown_editor();
}

static void test_point_goto_and_buffer_name(void)
{
	char result[128] = "";

	setup_editor();
	editor_insert_row(0, "zero", 4);
	editor_insert_row(1, "second", 6);
	editor_insert_row(2, "third", 5);
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_eval_string("(kg-goto 2 3)", 13, result, sizeof(result))
	    == 0);
	CHECK(
	    kg_lisp_eval_string("(kg-point)", 10, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "(2 3)") == 0);
	CHECK(
	    kg_lisp_eval_string("(kg-buffer-name)", 16, result, sizeof(result))
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

static void test_kg_load(void)
{
	char root[64], path[512], source[600], result[128] = "";
	int length;

	CHECK(setup_config_root(root, sizeof(root)) == 0);
	CHECK(kg_lisp_init() == 0);

	/* Bare names resolve to <config>/kg/lisp/NAME.fe. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg.fe", root);
	CHECK(write_text_file(path, "(= pkg-value 7)\n") == 0);
	CHECK(
	    kg_lisp_eval_string("(kg-load \"pkg\")", 15, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("pkg-value", 9, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "7") == 0);

	/* Missing packages raise an error naming the resolved path. */
	CHECK(kg_lisp_eval_string(
		  "(kg-load \"absent\")", 18, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "absent.fe") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 1 1)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "2") == 0);

	/* Packages may load packages. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-a.fe", root);
	CHECK(
	    write_text_file(path, "(kg-load \"pkg-b\")\n(= a-after b-value)\n")
	    == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-b.fe", root);
	CHECK(write_text_file(path, "(= b-value 5)\n") == 0);
	CHECK(kg_lisp_eval_string(
		  "(kg-load \"pkg-a\")", 17, result, sizeof(result))
	    == 0);
	CHECK(kg_lisp_eval_string("a-after", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "5") == 0);

	/* Load cycles hit the depth limit and recover. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/pkg-x.fe", root);
	CHECK(write_text_file(path, "(kg-load \"pkg-x\")\n") == 0);
	CHECK(kg_lisp_eval_string(
		  "(kg-load \"pkg-x\")", 17, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "depth limit") != nullptr);
	CHECK(kg_lisp_eval_string("(+ 2 2)", 7, result, sizeof(result)) == 0);
	CHECK(strcmp(result, "4") == 0);

	/* Names containing '/' are literal paths. */
	(void)snprintf(path, sizeof(path), "%s/direct.fe", root);
	CHECK(write_text_file(path, "(= direct-value 3)\n") == 0);
	length = snprintf(source, sizeof(source), "(kg-load \"%s\")", path);
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
	CHECK(kg_lisp_eval_string(
		  "(kg-command \"shell-command\")", 28, result, sizeof(result))
	    != 0);
	CHECK(strstr(result, "not allowed") != nullptr);
	CHECK(strstr(result, "shell-command") != nullptr);
	CHECK(test_command_calls == 0);
	CHECK(kg_lisp_eval_string(
		  "(kg-command \"version\")", 22, result, sizeof(result))
	    == 0);
	CHECK(test_command_calls == 1);
	CHECK(strcmp(test_command_name, "version") == 0);
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
	RUN(test_point_goto_and_buffer_name);
	RUN(test_command_allow_list);
	RUN(test_init_file);
	RUN(test_kg_load);
	return test_summary();
}
