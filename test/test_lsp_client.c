#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp, setenv, realpath under -std=c2x */
#endif

/* test_lsp_client.c — the JSON-RPC client and the server registry above it
 * (src/lsp_client.c, src/lsp_server.c; Stage 3 of
 * doc/plans/2026-08-08-lsp.md).
 *
 * Every protocol case drives a real child: test/fake_lsp_server.py in its
 * `protocol` mode, whose answers are canned by argv, so an assertion is
 * against a value this file chose.  That is the point of the arrangement --
 * the defects a JSON-RPC client has are defects of a conversation (a
 * response matched to the wrong request, a callback that never runs because
 * the server died, a server request left hanging) and none of them is
 * reachable without two processes.
 *
 * The registry cases are the other half: what gets spawned, where a
 * workspace starts, and which instances are shared.  Root detection needs
 * no child at all, only a temporary tree; the instance cases use the
 * environment override with a command cheaper than a language server.
 *
 * Nothing sleeps blind.  Every wait is a pump loop with a deadline, so a
 * case that would hang fails in a few seconds naming the condition it was
 * waiting for.
 */

#include "../src/lsp_client.h"
#include "../src/lsp_json.h"
#include "../src/lsp_server.h"
#include "../src/process.h"
#include "test.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Long enough that a loaded box or a valgrind lane never trips it, short
 * enough that a genuine hang is reported rather than waited out. */
#define PUMP_DEADLINE_SECONDS 10.0

static char script_path[PATH_MAX];

/* The temporary trees the root-detection and registry cases walk. */
static char tree[PATH_MAX];
static char bare[PATH_MAX];

static double monotonic_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------ the fake ------------------------------ */

static struct lsp_client *start_protocol(const char *const *extra)
{
	const char *argv[24];
	struct kg_spawn_request req = { .stdin_fd = -1 };
	int n = 0;
	int i;

	argv[n++] = "python3";
	argv[n++] = script_path;
	argv[n++] = "--mode";
	argv[n++] = "protocol";
	for (i = 0; extra && extra[i] && n < 23; i++) {
		argv[n++] = extra[i];
	}
	argv[n] = NULL;
	req.argv = argv;
	return lsp_client_start(&req, "/tmp");
}

/* Poll until the client reaches `want`, or the deadline passes.  Returns
 * the state it ended on, so a failing case reports what it got. */
static enum lsp_client_state pump_until_state(
    struct lsp_client *c, enum lsp_client_state want)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	struct timespec nap = { 0, 1000000 }; /* 1 ms */

	while (lsp_client_state(c) != want && monotonic_seconds() < deadline) {
		(void)lsp_client_poll(c);
		nanosleep(&nap, NULL);
	}
	return lsp_client_state(c);
}

/* Poll until every registered callback has run, or the client died, or the
 * deadline passes.  Returns how many are still outstanding. */
static size_t pump_until_answered(struct lsp_client *c)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	struct timespec nap = { 0, 1000000 };

	while (
	    lsp_client_pending_count(c) > 0 && monotonic_seconds() < deadline) {
		(void)lsp_client_poll(c);
		nanosleep(&nap, NULL);
	}
	return lsp_client_pending_count(c);
}

/* What a callback saw: enough to prove the right answer reached the right
 * caller, and nothing that outlives the borrowed nodes. */
struct answer {
	int calls;
	bool had_result;
	bool had_error;
	char tag[64];
	bool method_not_found;
};

static void record(struct lsp_client *c, const struct lsp_json_value *result,
    const struct lsp_json_value *error, void *ctx)
{
	struct answer *a = ctx;
	const char *tag;

	(void)c;
	a->calls++;
	a->had_result = result != NULL;
	a->had_error = error != NULL;
	tag = lsp_json_str(lsp_json_get(result, "tag"), NULL);
	if (tag) {
		snprintf(a->tag, sizeof(a->tag), "%s", tag);
	}
	a->method_not_found
	    = lsp_json_bool(lsp_json_get(result, "methodNotFound"), false);
}

static long long echo_request(
    struct lsp_client *c, const char *tag, struct answer *out)
{
	char params[64];
	int n = snprintf(params, sizeof(params), "{\"tag\":\"%s\"}", tag);

	return lsp_client_request(c, "kg/echo", params, (size_t)n, record, out);
}

/* ---------------------------- protocol cases -------------------------- */

/* The handshake, and the two capabilities kg keeps from it.  A server that
 * says nothing about position encoding gets the protocol's default, which
 * is the fallback every conversion in Stage 4 has to handle. */
static void test_handshake_defaults_to_utf16(void)
{
	const char *extra[] = { "--sync", "full", NULL };
	struct lsp_client *c = start_protocol(extra);
	const struct lsp_capabilities *caps;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	/* The child is spawned but has answered nothing yet. */
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	caps = lsp_client_caps(c);
	CHECK(caps->position_encoding == LSP_POSITION_UTF16);
	CHECK(caps->sync == LSP_SYNC_FULL);
	CHECK(caps->open_close);
	CHECK(strcmp(lsp_client_root(c), "/tmp") == 0);
	lsp_client_dispose(c, 200);
}

static void test_handshake_captures_utf8_and_incremental(void)
{
	const char *extra[]
	    = { "--position-encoding", "utf-8", "--sync", "incremental", NULL };
	struct lsp_client *c = start_protocol(extra);
	const struct lsp_capabilities *caps;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	caps = lsp_client_caps(c);
	CHECK(caps->position_encoding == LSP_POSITION_UTF8);
	CHECK(caps->sync == LSP_SYNC_INCREMENTAL);
	lsp_client_dispose(c, 200);
}

/* Two requests in flight, answered in the opposite order: a client that
 * matched responses to callbacks by arrival would hand each one the other's
 * payload, and this is the case that would say so. */
static void test_responses_are_matched_by_id(void)
{
	const char *extra[] = { "--reverse-pairs", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer first = { 0 };
	struct answer second = { 0 };
	long long id_a, id_b;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	id_a = echo_request(c, "alpha", &first);
	id_b = echo_request(c, "beta", &second);
	CHECK(id_a > 0 && id_b > 0 && id_a != id_b);
	CHECK(lsp_client_pending_count(c) == 2);
	CHECK(pump_until_answered(c) == 0);
	CHECK(first.calls == 1 && strcmp(first.tag, "alpha") == 0);
	CHECK(second.calls == 1 && strcmp(second.tag, "beta") == 0);
	CHECK(first.had_result && !first.had_error);
	lsp_client_dispose(c, 200);
}

/* A request made before the handshake finished is held, not dropped and not
 * sent early -- a server may answer nothing before it is initialized. */
static void test_requests_before_ready_are_flushed(void)
{
	struct lsp_client *c = start_protocol(NULL);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(echo_request(c, "queued", &got) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(lsp_client_state(c) == LSP_CLIENT_READY);
	CHECK(got.calls == 1 && strcmp(got.tag, "queued") == 0);
	lsp_client_dispose(c, 200);
}

/* A server that dies with a request outstanding.  The callback must still
 * run -- once, with no result -- or every caller's context leaks and every
 * command waits forever. */
static void test_server_death_fails_pending_requests(void)
{
	const char *extra[] = { "--die-on", "kg/echo", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "doomed", &got) > 0);
	CHECK(pump_until_state(c, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
	CHECK(got.calls == 1);
	CHECK(!got.had_result);
	CHECK(got.had_error); /* the synthesised one, so callers can print it */
	CHECK(lsp_client_pending_count(c) == 0);
	/* A dead client refuses rather than pretending. */
	CHECK(echo_request(c, "after", &got) == -1);
	CHECK(got.calls == 1);
	lsp_client_dispose(c, 200);
}

/* kg implements no server-to-client requests, and answering that with
 * silence is what makes a server stop answering kg.  The fake sends one
 * before its first reply -- while the client is still INITIALIZING, which
 * is when a real server asks for configuration -- and reports through
 * kg/state whether it got the MethodNotFound error back. */
static void test_server_request_is_refused_not_ignored(void)
{
	const char *extra[]
	    = { "--server-request", "workspace/configuration", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer state = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(lsp_client_request(c, "kg/state", NULL, 0, record, &state) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(state.calls == 1 && state.had_result);
	CHECK(state.method_not_found);
	lsp_client_dispose(c, 200);
}

/* A notification kg did not ask for is dropped, and the session carries on:
 * the failure this guards against is a client that treats an unknown
 * message as a protocol violation and kills a healthy server. */
static void test_unsolicited_notification_is_ignored(void)
{
	const char *extra[] = { "--notify", "window/logMessage", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "still-here", &got) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(got.calls == 1 && strcmp(got.tag, "still-here") == 0);
	CHECK(lsp_client_state(c) == LSP_CLIENT_READY);
	lsp_client_dispose(c, 200);
}

/* shutdown, then exit, then the server is gone -- and the client notices
 * that by itself, which is what makes a graceful exit distinguishable from
 * a crash. */
static void test_shutdown_handshake_ends_the_server(void)
{
	struct lsp_client *c = start_protocol(NULL);

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	lsp_client_shutdown_begin(c);
	CHECK(pump_until_state(c, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
	CHECK(lsp_client_pending_count(c) == 0);
	lsp_client_dispose(c, 200);
}

/* Disposing with an answer still outstanding.  --reverse-pairs holds a lone
 * kg/echo forever, which is a server that simply never replies; the
 * callback must run anyway, so nothing the caller allocated is orphaned.
 * This is also the case valgrind is pointed at. */
static void test_dispose_runs_pending_callbacks(void)
{
	const char *extra[] = { "--reverse-pairs", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "unanswered", &got) > 0);
	CHECK(lsp_client_pending_count(c) == 1);
	lsp_client_dispose(c, 50);
	CHECK(got.calls == 1);
	CHECK(!got.had_result);
}

/* A spawn that cannot happen is a NULL, not a client that never answers. */
static void test_start_failure_is_reported(void)
{
	const char *argv[2] = { "kg-no-such-language-server", NULL };
	struct kg_spawn_request req = { .argv = argv, .stdin_fd = -1 };
	struct lsp_client *c = lsp_client_start(&req, "/tmp");

	/* execvp() fails in the child, so the spawn itself may succeed and
	 * the death shows up as an immediate end of stream. */
	if (c) {
		CHECK(pump_until_state(c, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
		lsp_client_dispose(c, 50);
	}
}

/* -------------------------- workspace roots --------------------------- */

static void path_of(char *out, size_t n, const char *base, const char *rel)
{
	if (rel && *rel) {
		snprintf(out, n, "%s/%s", base, rel);
	} else {
		snprintf(out, n, "%s", base);
	}
}

static void mk_dir(const char *base, const char *rel)
{
	char path[PATH_MAX];

	path_of(path, sizeof(path), base, rel);
	CHECKF(mkdir(path, 0700) == 0, "mkdir %s", path);
}

static void mk_file(const char *base, const char *rel, const char *content)
{
	char path[PATH_MAX];
	FILE *fp;

	path_of(path, sizeof(path), base, rel);
	fp = fopen(path, "wb");
	CHECKF(fp != NULL, "fopen %s", path);
	if (!fp) {
		return;
	}
	fputs(content, fp);
	fclose(fp);
}

static void check_root(
    enum kg_mode_id mode, const char *file_rel, const char *want_rel)
{
	char file[PATH_MAX];
	char want[PATH_MAX];
	char got[PATH_MAX] = { 0 };

	path_of(file, sizeof(file), tree, file_rel);
	path_of(want, sizeof(want), tree, want_rel);
	CHECKF(lsp_workspace_root(mode, file, got, sizeof(got)), "%s", file);
	CHECKF(
	    strcmp(got, want) == 0, "%s: got '%s', want '%s'", file, got, want);
}

/* The tree every root case reads.  Built once, in main(), because these
 * cases are about which ancestor wins and that is only visible in a tree
 * with several candidates. */
static void build_tree(void)
{
	mk_dir(tree, ".git");
	mk_dir(tree, "proj");
	mk_file(tree, "proj/compile_commands.json", "[]\n");
	mk_dir(tree, "proj/deep");
	mk_dir(tree, "bld");
	mk_dir(tree, "bld/build");
	mk_file(tree, "bld/build/compile_commands.json", "[]\n");
	mk_dir(tree, "flags");
	mk_file(tree, "flags/compile_flags.txt", "-I.\n");
	mk_dir(tree, "plain");
	mk_dir(tree, "py");
	mk_file(tree, "py/pyproject.toml", "[project]\nname='x'\n[tool.ty]\n");
	mk_dir(tree, "py/mod");
	mk_dir(tree, "other");
	mk_file(tree, "other/pyproject.toml", "[project]\nname='y'\n");
}

/* The nearest ancestor with a C marker wins over the .git further up: a
 * root chosen higher than the compilation database is one clangd will
 * disagree with about every include path. */
static void test_root_prefers_nearest_c_marker(void)
{
	check_root(KG_MODE_C, "proj/deep/a.c", "proj");
	check_root(KG_MODE_C, "bld/x.c", "bld");
	check_root(KG_MODE_C, "flags/x.c", "flags");
}

/* No marker of this mode anywhere: .git is the fallback, and it is checked
 * from the file's own directory upwards, not from where the first pass
 * stopped. */
static void test_root_falls_back_to_git(void)
{
	check_root(KG_MODE_C, "plain/a.c", "");
	check_root(KG_MODE_PYTHON, "proj/deep/a.py", "");
}

/* pyproject.toml counts only when it mentions ty: a Python project with
 * some other type checker is not a ty workspace, and rooting there would
 * hide the .git the rest of the sources live under. */
static void test_root_reads_pyproject_for_tool_ty(void)
{
	check_root(KG_MODE_PYTHON, "py/mod/x.py", "py");
	check_root(KG_MODE_PYTHON, "other/x.py", "");
	/* The same file in C mode ignores the Python markers entirely. */
	check_root(KG_MODE_C, "py/mod/x.c", "");
}

/* Nothing above the file at all: its own directory is the root, so a
 * scratch file outside any project still gets a server. */
static void test_root_defaults_to_the_files_directory(void)
{
	char got[PATH_MAX] = { 0 };
	char file[PATH_MAX];

	path_of(file, sizeof(file), bare, "loose.c");
	CHECK(lsp_workspace_root(KG_MODE_C, file, got, sizeof(got)));
	CHECK(strcmp(got, bare) == 0);
}

/* A relative path has no workspace to find, and saying so is what makes the
 * caller's error message honest. */
static void test_root_refuses_a_relative_path(void)
{
	char got[PATH_MAX] = { 0 };

	CHECK(!lsp_workspace_root(KG_MODE_C, "src/main.c", got, sizeof(got)));
	CHECK(!lsp_workspace_root(KG_MODE_C, "", got, sizeof(got)));
	CHECK(!lsp_workspace_root(KG_MODE_C, NULL, got, sizeof(got)));
}

/* ----------------------------- the registry --------------------------- */

/* A command that reads its input and answers nothing, which is all a
 * registry case needs: the instance exists and stays INITIALIZING, at the
 * cost of one `cat` instead of one language server. */
static void set_c_server(const char *command)
{
	if (command) {
		setenv("KG_LSP_SERVER_C", command, 1);
	} else {
		unsetenv("KG_LSP_SERVER_C");
	}
}

static struct lsp_client *server_for(
    const char *file_rel, enum lsp_server_status *status)
{
	char file[PATH_MAX];

	path_of(file, sizeof(file), tree, file_rel);
	return lsp_server_for(KG_MODE_C, file, status);
}

/* One server per (mode, root): two files under the same root share an
 * instance, two roots do not.  That is the whole reason the registry is
 * keyed by root rather than by buffer. */
static void test_registry_keys_instances_by_root(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	struct lsp_client *a;
	struct lsp_client *b;
	struct lsp_client *other;

	set_c_server("cat >/dev/null");
	a = server_for("proj/deep/a.c", &status);
	CHECK(a != NULL && status == LSP_SERVER_OK);
	b = server_for("proj/deep/b.c", &status);
	CHECK(b == a);
	CHECK(lsp_server_instance_count() == 1);

	other = server_for("bld/x.c", &status);
	CHECK(other != NULL && other != a);
	CHECK(lsp_server_instance_count() == 2);

	lsp_server_shutdown_all(200);
	CHECK(lsp_server_instance_count() == 0);
	set_c_server(NULL);
}

/* A server that died is replaced on the next request rather than restarted
 * where it fell -- the lazy-restart policy.  The client pointer may be
 * reused by the allocator, so the assertion is about state: what comes back
 * is never a dead instance. */
static void test_registry_replaces_a_dead_instance(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	struct lsp_client *first;
	struct lsp_client *second;

	set_c_server("exit 0");
	first = server_for("proj/deep/a.c", &status);
	CHECK(first != NULL);
	if (!first) {
		set_c_server(NULL);
		return;
	}
	CHECK(pump_until_state(first, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
	CHECK(lsp_server_instance_count() == 1);

	second = server_for("proj/deep/a.c", &status);
	CHECK(second != NULL);
	CHECK(second && lsp_client_state(second) != LSP_CLIENT_DEAD);
	CHECK(lsp_server_instance_count() == 1);
	lsp_server_shutdown_all(200);
	set_c_server(NULL);
}

/* The bound is a refusal with a reason, not a slower editor. */
static void test_registry_refuses_a_fifth_instance(void)
{
	static const char *const roots[]
	    = { "r1", "r2", "r3", "r4", "r5", NULL };
	enum lsp_server_status status = LSP_SERVER_OK;
	char rel[64];
	int i;

	set_c_server("cat >/dev/null");
	for (i = 0; roots[i]; i++) {
		mk_dir(tree, roots[i]);
		snprintf(
		    rel, sizeof(rel), "%s/compile_commands.json", roots[i]);
		mk_file(tree, rel, "[]\n");
		snprintf(rel, sizeof(rel), "%s/a.c", roots[i]);
		if (i < LSP_SERVER_MAX_INSTANCES) {
			CHECKF(server_for(rel, &status) != NULL, "%s", rel);
			continue;
		}
		CHECK(server_for(rel, &status) == NULL);
		CHECK(status == LSP_SERVER_REGISTRY_FULL);
	}
	CHECK(lsp_server_instance_count() == LSP_SERVER_MAX_INSTANCES);
	CHECK(strstr(lsp_server_status_text(LSP_SERVER_REGISTRY_FULL), "many")
	    != NULL);
	lsp_server_shutdown_all(400);
	CHECK(lsp_server_instance_count() == 0);
	set_c_server(NULL);
}

/* Most modes have no server, and that is an answer rather than an error:
 * nothing is spawned and the caller gets something to print. */
static void test_registry_refuses_an_unsupported_mode(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	char file[PATH_MAX];

	path_of(file, sizeof(file), tree, "plain/a.txt");
	CHECK(lsp_server_for(KG_MODE_TEXT, file, &status) == NULL);
	CHECK(status == LSP_SERVER_UNSUPPORTED_MODE);
	CHECK(lsp_server_instance_count() == 0);
	/* A NULL status out-parameter is accepted. */
	CHECK(lsp_server_for(KG_MODE_TEXT, file, NULL) == NULL);
}

/* The override is a shell command line, so a wrapper with arguments and
 * quoting works -- which is also how every one of these cases injects a
 * server.  Here it is the real fake, driven to a full handshake through the
 * registry. */
static void test_env_override_spawns_the_fake_server(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	char command[PATH_MAX + 128];
	struct lsp_client *c;

	snprintf(command, sizeof(command),
	    "python3 '%s' --mode protocol --position-encoding utf-8",
	    script_path);
	set_c_server(command);
	c = server_for("proj/deep/a.c", &status);
	CHECK(c != NULL && status == LSP_SERVER_OK);
	if (c) {
		CHECK(
		    pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
		CHECK(
		    lsp_client_caps(c)->position_encoding == LSP_POSITION_UTF8);
		/* The registry's own poll is what the editor calls. */
		(void)lsp_server_poll_all();
	}
	lsp_server_shutdown_all(300);
	CHECK(lsp_server_instance_count() == 0);
	set_c_server(NULL);
}

/* ------------------------------- harness ------------------------------ */

/* Where test/fake_lsp_server.py is, given how this binary was invoked:
 * beside it in test/ for `make check` and for a hand-run valgrind, with the
 * repo-relative path as the fallback for a runner that renames the
 * binary. */
static void resolve_script_path(const char *argv0)
{
	const char *slash = strrchr(argv0, '/');
	int dir_len = slash ? (int)(slash - argv0) + 1 : 0;
	char resolved[PATH_MAX];

	snprintf(script_path, sizeof(script_path), "%.*sfake_lsp_server.py",
	    dir_len, argv0);
	if (access(script_path, R_OK) != 0) {
		snprintf(script_path, sizeof(script_path),
		    "test/fake_lsp_server.py");
	}
	/* Absolute, because the registry cases spawn it with the workspace
	 * root as the child's working directory. */
	if (realpath(script_path, resolved)) {
		snprintf(script_path, sizeof(script_path), "%s", resolved);
	}
}

static bool make_trees(void)
{
	char template[] = "/tmp/kg-lsp-root-XXXXXX";
	char bare_template[] = "/tmp/kg-lsp-bare-XXXXXX";
	char resolved[PATH_MAX];

	if (!mkdtemp(template) || !mkdtemp(bare_template)) {
		return false;
	}
	/* realpath, because a /tmp that is itself a symlink would otherwise
	 * make every expectation here disagree with what the walk returns. */
	snprintf(tree, sizeof(tree), "%s",
	    realpath(template, resolved) ? resolved : template);
	snprintf(bare, sizeof(bare), "%s",
	    realpath(bare_template, resolved) ? resolved : bare_template);
	return true;
}

static void remove_trees(void)
{
	char command[2 * PATH_MAX + 32];

	snprintf(command, sizeof(command), "rm -rf '%s' '%s'", tree, bare);
	if (system(command) != 0) {
		fprintf(stderr, "test_lsp_client: could not remove %s\n", tree);
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	resolve_script_path(argv[0]);
	if (access(script_path, R_OK) != 0) {
		fprintf(stderr,
		    "test_lsp_client: cannot find fake_lsp_server.py "
		    "(tried '%s')\n",
		    script_path);
		return 1;
	}
	if (!make_trees()) {
		fprintf(stderr, "test_lsp_client: mkdtemp failed\n");
		return 1;
	}
	build_tree();

	RUN(test_handshake_defaults_to_utf16);
	RUN(test_handshake_captures_utf8_and_incremental);
	RUN(test_responses_are_matched_by_id);
	RUN(test_requests_before_ready_are_flushed);
	RUN(test_server_death_fails_pending_requests);
	RUN(test_server_request_is_refused_not_ignored);
	RUN(test_unsolicited_notification_is_ignored);
	RUN(test_shutdown_handshake_ends_the_server);
	RUN(test_dispose_runs_pending_callbacks);
	RUN(test_start_failure_is_reported);

	RUN(test_root_prefers_nearest_c_marker);
	RUN(test_root_falls_back_to_git);
	RUN(test_root_reads_pyproject_for_tool_ty);
	RUN(test_root_defaults_to_the_files_directory);
	RUN(test_root_refuses_a_relative_path);

	RUN(test_registry_keys_instances_by_root);
	RUN(test_registry_replaces_a_dead_instance);
	RUN(test_registry_refuses_a_fifth_instance);
	RUN(test_registry_refuses_an_unsupported_mode);
	RUN(test_env_override_spawns_the_fake_server);

	remove_trees();
	return test_summary();
}
