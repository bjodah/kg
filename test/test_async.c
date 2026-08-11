#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp, setenv, realpath under -std=c23 */
#endif

/* test_async.c -- the fixed editor_async_* subsystem aggregate. */

#include "../src/async.h"
#include "../src/lsp.h"
#include "test.h"

#include <limits.h>
#include <string.h>

#ifdef KG_USE_LSP
#include "../src/lsp_client.h"
#include "../src/lsp_server.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#endif

static void test_invalid_destinations_write_nothing(void)
{
	int fds[KG_ASYNC_WAIT_FDS_MAX];
	int before[KG_ASYNC_WAIT_FDS_MAX];

	memset(fds, 0x5a, sizeof(fds));
	memcpy(before, fds, sizeof(fds));
	CHECK(editor_async_wait_fds(NULL, KG_ASYNC_WAIT_FDS_MAX) == -1);
	CHECK(editor_async_wait_fds(fds, -1) == -1);
	CHECK(memcmp(fds, before, sizeof(fds)) == 0);
	CHECK(editor_async_wait_fds(fds, KG_ASYNC_WAIT_FDS_MAX - 1) == -1);
	CHECK(memcmp(fds, before, sizeof(fds)) == 0);
}

static void test_no_live_subsystem_is_a_valid_empty_wait(void)
{
	int fds[KG_ASYNC_WAIT_FDS_MAX];

	CHECK(editor_async_wait_fds(fds, KG_ASYNC_WAIT_FDS_MAX) == 0);
	CHECK(editor_async_poll() == 0);
}

#ifdef KG_USE_LSP
static double monotonic_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int make_file(const char *path, const char *text)
{
	FILE *fp = fopen(path, "w");
	int failed;

	if (!fp) {
		return -1;
	}
	failed = fputs(text, fp) == EOF;
	return fclose(fp) == 0 && !failed ? 0 : -1;
}

/* A real protocol child supplies the descriptors: the aggregate count and
 * order must equal the subsystem's, and one of those descriptors must end
 * a blocking wait rather than waiting for an editor idle tick. */
static void test_live_lsp_is_aggregated_and_wakes_the_wait(void)
{
	char template[] = "/tmp/kg-async-XXXXXX";
	char database[sizeof(template) + sizeof("/compile_commands.json")] = "";
	char source[sizeof(template) + sizeof("/main.c")] = "";
	char script[PATH_MAX];
	char command[PATH_MAX + 96];
	int async_fds[KG_ASYNC_WAIT_FDS_MAX];
	int lsp_fds[KG_LSP_WAIT_FDS_MAX];
	struct pollfd pfd[KG_ASYNC_WAIT_FDS_MAX];
	struct lsp_client *client = NULL;
	enum lsp_server_status status = LSP_SERVER_OK;
	double deadline;
	int async_count;
	int lsp_count;
	int ready = 0;
	int woke = 0;
	int changed = 0;
	int i;

	CHECK(mkdtemp(template) != NULL);
	(void)snprintf(
	    database, sizeof(database), "%s/compile_commands.json", template);
	(void)snprintf(source, sizeof(source), "%s/main.c", template);
	CHECK(make_file(database, "[]\n") == 0);
	CHECK(make_file(source, "int main(void) { return 0; }\n") == 0);
	CHECK(realpath("test/fake_lsp_server.py", script) != NULL);
	(void)snprintf(
	    command, sizeof(command), "python3 '%s' --mode protocol", script);
	CHECK(setenv("KG_LSP_SERVER_C", command, 1) == 0);
	client = lsp_server_for(KG_MODE_C, source, &status);
	CHECK(client != NULL && status == LSP_SERVER_OK);
	if (!client) {
		goto out;
	}

	lsp_count = lsp_wait_fds(lsp_fds, KG_LSP_WAIT_FDS_MAX);
	async_count = editor_async_wait_fds(async_fds, KG_ASYNC_WAIT_FDS_MAX);
	CHECK(async_count == lsp_count);
	CHECK(async_count > 0);
	for (i = 0; i < async_count; i++) {
		CHECK(async_fds[i] == lsp_fds[i]);
		pfd[i].fd = async_fds[i];
		pfd[i].events = POLLIN;
		pfd[i].revents = 0;
	}

	deadline = monotonic_seconds() + 10.0;
	while (!ready && monotonic_seconds() < deadline) {
		int rc = poll(pfd, (unsigned long)async_count, 1000);

		if (rc > 0) {
			woke = 1;
		}
		changed |= editor_async_poll();
		ready = lsp_client_state(client) == LSP_CLIENT_READY;
		async_count
		    = editor_async_wait_fds(async_fds, KG_ASYNC_WAIT_FDS_MAX);
		for (i = 0; i < async_count; i++) {
			pfd[i].fd = async_fds[i];
			pfd[i].events = POLLIN;
			pfd[i].revents = 0;
		}
	}
	CHECK(woke);
	CHECK(ready);
	CHECK(changed);
	CHECK(editor_async_poll() == 0);

out:
	lsp_server_shutdown_all(300);
	(void)unsetenv("KG_LSP_SERVER_C");
	if (source[0]) {
		(void)unlink(source);
	}
	if (database[0]) {
		(void)unlink(database);
	}
	(void)rmdir(template);
}
#endif /* KG_USE_LSP */

int main(void)
{
	RUN(test_invalid_destinations_write_nothing);
	RUN(test_no_live_subsystem_is_a_valid_empty_wait);
#ifdef KG_USE_LSP
	RUN(test_live_lsp_is_aggregated_and_wakes_the_wait);
#endif
	return test_summary();
}
