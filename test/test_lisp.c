/* test_lisp.c - Fe interpreter lifecycle regression tests */

#include "../src/lisp.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_disabled(void)
{
	char result[128] = "";

	CHECK(kg_lisp_init() != 0);
	CHECK(kg_lisp_eval_string("(+ 1 2)", 7, result, sizeof(result)) != 0);
	CHECK(strstr(result, "not compiled in") != nullptr);
	CHECK(kg_lisp_load_file("unused.fe") != 0);
	kg_lisp_shutdown();
	kg_lisp_shutdown();
}

static void test_lifecycle(void)
{
	char result[128] = "";

	kg_lisp_shutdown();
	CHECK(kg_lisp_eval_string("1", 1, result, sizeof(result)) != 0);
	CHECK(strstr(result, "not initialized") != nullptr);
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_init() == 0);
	kg_lisp_shutdown();
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
	CHECK(kg_lisp_eval_string("loaded-value", 12, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "41") == 0);
	kg_lisp_shutdown();
	CHECK(unlink(path) == 0);
}

int main(void)
{
	if (!kg_lisp_active()) {
		RUN(test_disabled);
		return test_summary();
	}

	RUN(test_lifecycle);
	RUN(test_eval_and_recovery);
	RUN(test_load_file);
	return test_summary();
}
