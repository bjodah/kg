/* ===================== Server specs and the instance registry ===========
 *
 * Policy above the protocol: which binary answers for which mode, where a
 * workspace starts, and which servers are already running.  See
 * src/lsp_server.h for the contract and doc/plans/2026-08-08-lsp.md for why
 * the registry is keyed by (server, workspace root) and started lazily.
 *
 * The whole file is a table and a walk.  The table says what to spawn; the
 * walk says where -- and both are deliberately dumb, because a root
 * detector that is clever is one that finds a different root than the
 * server does, and then every position kg sends is measured against a
 * different tree than the one the answers come from.
 *
 * Nothing here includes def.h: syntax.h for the mode ids, the client for
 * the protocol, process.h for the spawn request, POSIX for the file
 * system.  A test binary links it with no editor object at all.
 */

#include "lsp_server.h"

#include "lsp_client.h"
#include "process.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The smallest grace any one server gets when the editor is exiting, even
 * if the budget divided between them would be less.  Below this the wait is
 * not a graceful shutdown, it is a formality before the kill. */
#define LSP_SERVER_MIN_GRACE_MS 10u

static bool dir_has_c_markers(const char *dir);
static bool dir_has_python_markers(const char *dir);
static bool dir_has_go_markers(const char *dir);
static bool dir_has_rust_markers(const char *dir);

/* What kg knows how to start.  `env` is spelled out rather than derived
 * from `name` so the environment variable a user sets is greppable in this
 * file, and `argv` is a direct exec -- no shell -- so nothing in a built-in
 * spec can be word-split by a stray space in the environment.  The override
 * is the opposite by design: a command line for /bin/sh. */
struct lsp_server_spec {
	enum kg_mode_id mode;
	const char *name;
	const char *env;
	const char *const argv[4];
	bool (*has_markers)(const char *dir);
};

static const struct lsp_server_spec server_specs[] = {
	{ KG_MODE_C, "clangd", LSP_SERVER_ENV_PREFIX "C", { "clangd", NULL },
	    dir_has_c_markers },
	{ KG_MODE_PYTHON, "ty", LSP_SERVER_ENV_PREFIX "PYTHON",
	    { "ty", "server", NULL }, dir_has_python_markers },
	{ KG_MODE_GO, "gopls", LSP_SERVER_ENV_PREFIX "GO", { "gopls", NULL },
	    dir_has_go_markers },
	{ KG_MODE_RUST, "rust-analyzer", LSP_SERVER_ENV_PREFIX "RUST",
	    { "rust-analyzer", NULL }, dir_has_rust_markers },
};

struct lsp_instance {
	const struct lsp_server_spec *spec;
	struct lsp_client *client;
	char root[PATH_MAX];
};

static struct lsp_instance instances[LSP_SERVER_MAX_INSTANCES];

/* Whoever wants to be told that a client is about to be disposed of; see
 * src/lsp_server.h.  One hook, not a list: there is exactly one thing in kg
 * that keeps per-client state, and a registry of listeners for a single
 * listener is a table to get wrong. */
static lsp_instance_drop_fn instance_drop_hook;

void lsp_server_set_instance_drop_hook(lsp_instance_drop_fn fn)
{
	instance_drop_hook = fn;
}

/* Dispose of a slot's client and empty the slot, telling the hook first.
 * Every path that ends a client goes through here, so there is no way to
 * free one without whoever kept state about it hearing so. */
static void instance_drop(struct lsp_instance *slot, unsigned grace_ms)
{
	if (instance_drop_hook) {
		instance_drop_hook(slot->client);
	}
	lsp_client_dispose(slot->client, grace_ms);
	memset(slot, 0, sizeof(*slot));
}

static const struct lsp_server_spec *spec_for(enum kg_mode_id mode)
{
	size_t i;

	for (i = 0; i < sizeof(server_specs) / sizeof(server_specs[0]); i++) {
		if (server_specs[i].mode == mode) {
			return &server_specs[i];
		}
	}
	return NULL;
}

/* --------------------------- workspace roots -------------------------- */

/* `dir/name` exists, whatever it is: a directory, a regular file, or the
 * file `git worktree` leaves where a `.git` directory would be.  The
 * markers are all "this is the top of something", and none of them is worth
 * a type check the next build tool would invalidate. */
static bool marker_exists(const char *dir, const char *name)
{
	char path[PATH_MAX];
	struct stat st;
	int n = snprintf(path, sizeof(path), "%s/%s", dir, name);

	if (n < 0 || (size_t)n >= sizeof(path)) {
		return false;
	}
	return stat(path, &st) == 0;
}

/* Whether `dir/name` contains `needle` anywhere in its first bytes.  Bounded
 * and byte-exact: a pyproject.toml is read to answer one question, not
 * parsed, and a TOML parser here would be a second dependency answering the
 * same question worse. */
static bool marker_contains(
    const char *dir, const char *name, const char *needle)
{
	char path[PATH_MAX];
	char text[8192];
	size_t got;
	FILE *fp;
	int n = snprintf(path, sizeof(path), "%s/%s", dir, name);

	if (n < 0 || (size_t)n >= sizeof(path)) {
		return false;
	}
	fp = fopen(path, "rb");
	if (!fp) {
		return false;
	}
	got = fread(text, 1, sizeof(text) - 1, fp);
	fclose(fp);
	text[got] = '\0';
	return strstr(text, needle) != NULL;
}

static bool dir_has_c_markers(const char *dir)
{
	return marker_exists(dir, ".clangd")
	    || marker_exists(dir, "compile_commands.json")
	    || marker_exists(dir, "compile_flags.txt")
	    || marker_exists(dir, "build/compile_commands.json");
}

static bool dir_has_python_markers(const char *dir)
{
	return marker_exists(dir, "ty.toml")
	    || marker_contains(dir, "pyproject.toml", "[tool.ty");
}

/* Go.  Both spellings of "the top of something" are markers, and the walk
 * takes the nearest, which is the module: a go.work always sits at or above
 * the go.mod files it lists, so a file inside a module roots on its module
 * even in a workspace -- and that is the root gopls loads, because the go
 * command finds the go.work above it on its own (`go env GOWORK` walks up
 * from the working directory).  go.work alone wins only for a file that is
 * under a workspace but under no module, which is where the module answer
 * does not exist. */
static bool dir_has_go_markers(const char *dir)
{
	return marker_exists(dir, "go.mod") || marker_exists(dir, "go.work");
}

/* Rust.  A Cargo workspace has a Cargo.toml at its root and one per member
 * crate, so the nearest is the member rather than the workspace.  That is
 * still a root rust-analyzer agrees with: it runs `cargo metadata` from
 * where it is started, and cargo resolves the workspace above the member
 * itself.  Rooting at the member also keeps one server per crate on a tree
 * with several unrelated ones. */
static bool dir_has_rust_markers(const char *dir)
{
	return marker_exists(dir, "Cargo.toml");
}

static bool dir_has_git(const char *dir) { return marker_exists(dir, ".git"); }

/* Strip the last component in place.  False at the root, which is where
 * every walk stops. */
static bool parent_dir(char *dir)
{
	char *slash = strrchr(dir, '/');

	if (!slash || slash == dir) {
		return false;
	}
	*slash = '\0';
	return true;
}

/* The nearest ancestor of `start` (inclusive) that `pred` accepts.  The
 * walk is bounded rather than trusting the path to end: a path assembled
 * out of symlinks can be longer than any tree. */
static bool ancestor_matching(
    const char *start, bool (*pred)(const char *), char *out, size_t out_size)
{
	char dir[PATH_MAX];
	int levels;

	if (snprintf(dir, sizeof(dir), "%s", start) < 0) {
		return false;
	}
	for (levels = 0; levels < LSP_SERVER_MAX_WALK; levels++) {
		if (pred(dir)) {
			return (size_t)snprintf(out, out_size, "%s", dir)
			    < out_size;
		}
		if (!parent_dir(dir)) {
			return false;
		}
	}
	return false;
}

/* The directory `abs_path` lives in, canonicalised once.  realpath() is
 * asked about the directory and not the file, because the file may not
 * exist yet (a buffer visiting a path nobody has saved), and its failure is
 * not fatal: an uncanonical but absolute directory is still a root both kg
 * and the server can agree on. */
static bool start_dir_of(const char *abs_path, char *out, size_t out_size)
{
	char dir[PATH_MAX];
	char real[PATH_MAX];
	const char *slash;
	size_t len;

	if (!abs_path || abs_path[0] != '/') {
		return false;
	}
	slash = strrchr(abs_path, '/');
	len = (slash == abs_path) ? 1u : (size_t)(slash - abs_path);
	if (len >= sizeof(dir)) {
		return false;
	}
	memcpy(dir, abs_path, len);
	dir[len] = '\0';
	if (realpath(dir, real)) {
		return (size_t)snprintf(out, out_size, "%s", real) < out_size;
	}
	return (size_t)snprintf(out, out_size, "%s", dir) < out_size;
}

bool lsp_workspace_root(
    enum kg_mode_id mode, const char *abs_path, char *out, size_t out_size)
{
	const struct lsp_server_spec *spec = spec_for(mode);
	char dir[PATH_MAX];

	if (!start_dir_of(abs_path, dir, sizeof(dir))) {
		return false;
	}
	if (spec && ancestor_matching(dir, spec->has_markers, out, out_size)) {
		return true;
	}
	if (ancestor_matching(dir, dir_has_git, out, out_size)) {
		return true;
	}
	return (size_t)snprintf(out, out_size, "%s", dir) < out_size;
}

/* ------------------------------- registry ----------------------------- */

/* Start one server for `spec` in `root`.  The override wins and is a shell
 * command line (src/lsp_server.h says so to the user); the built-in spec is
 * an argv exec'd directly.  The child's working directory is the workspace
 * root, which is what a server started by hand in that directory would
 * see. */
static struct lsp_client *spec_start(
    const struct lsp_server_spec *spec, const char *root)
{
	const char *override = getenv(spec->env);
	struct kg_spawn_request req = {
		.directory = root,
		.stdin_fd = -1,
	};
	struct lsp_client *c;

	if (override && *override) {
		req.command = override;
	} else {
		req.argv = spec->argv;
	}
	c = lsp_client_start(&req, root);
	/* The spec's name, not the command line's: an override points kg at
	 * a wrapper script or a fake, and what a message about "clangd"
	 * should say is which server kg thinks it is talking to. */
	if (c) {
		lsp_client_set_name(c, spec->name);
	}
	return c;
}

/* The slot holding this key, or NULL.  A slot whose client has died is
 * emptied here rather than returned: reclaiming on the next request is what
 * makes the restart lazy, and doing it at the lookup means no caller ever
 * receives a DEAD client. */
static struct lsp_instance *instance_find(
    const struct lsp_server_spec *spec, const char *root)
{
	struct lsp_instance *slot;
	size_t i;

	for (i = 0; i < LSP_SERVER_MAX_INSTANCES; i++) {
		slot = &instances[i];
		if (slot->spec != spec || strcmp(slot->root, root) != 0) {
			continue;
		}
		if (lsp_client_state(slot->client) == LSP_CLIENT_DEAD) {
			instance_drop(slot, 0);
			return NULL;
		}
		return slot;
	}
	return NULL;
}

static struct lsp_instance *instance_free_slot(void)
{
	size_t i;

	for (i = 0; i < LSP_SERVER_MAX_INSTANCES; i++) {
		if (!instances[i].spec) {
			return &instances[i];
		}
	}
	return NULL;
}

static void status_set(enum lsp_server_status *out, enum lsp_server_status s)
{
	if (out) {
		*out = s;
	}
}

struct lsp_client *lsp_server_for(enum kg_mode_id mode, const char *abs_path,
    enum lsp_server_status *status_out)
{
	const struct lsp_server_spec *spec = spec_for(mode);
	struct lsp_instance *slot;
	char root[PATH_MAX];

	status_set(status_out, LSP_SERVER_OK);
	if (!spec) {
		status_set(status_out, LSP_SERVER_UNSUPPORTED_MODE);
		return NULL;
	}
	if (!lsp_workspace_root(mode, abs_path, root, sizeof(root))) {
		status_set(status_out, LSP_SERVER_NO_ROOT);
		return NULL;
	}
	slot = instance_find(spec, root);
	if (slot) {
		return slot->client;
	}
	slot = instance_free_slot();
	if (!slot) {
		status_set(status_out, LSP_SERVER_REGISTRY_FULL);
		return NULL;
	}
	slot->client = spec_start(spec, root);
	if (!slot->client) {
		status_set(status_out, LSP_SERVER_SPAWN_FAILED);
		return NULL;
	}
	slot->spec = spec;
	snprintf(slot->root, sizeof(slot->root), "%s", root);
	return slot->client;
}

const char *lsp_server_status_text(enum lsp_server_status status)
{
	switch (status) {
	case LSP_SERVER_OK:
		return "ok";
	case LSP_SERVER_UNSUPPORTED_MODE:
		return "no language server is configured for this mode";
	case LSP_SERVER_NO_ROOT:
		return "no workspace root for this buffer";
	case LSP_SERVER_REGISTRY_FULL:
		return "too many language servers are already running";
	case LSP_SERVER_SPAWN_FAILED:
		return "could not start the language server";
	}
	return "unknown language server error";
}

int lsp_server_poll_all(void)
{
	int changed = 0;
	size_t i;

	for (i = 0; i < LSP_SERVER_MAX_INSTANCES; i++) {
		if (instances[i].spec) {
			changed |= lsp_client_poll(instances[i].client);
		}
	}
	return changed;
}

void lsp_server_shutdown_all(unsigned grace_ms)
{
	unsigned live = (unsigned)lsp_server_instance_count();
	unsigned each;
	size_t i;

	if (live == 0) {
		return;
	}
	each = grace_ms / live;
	if (each < LSP_SERVER_MIN_GRACE_MS) {
		each = LSP_SERVER_MIN_GRACE_MS;
	}
	for (i = 0; i < LSP_SERVER_MAX_INSTANCES; i++) {
		if (!instances[i].spec) {
			continue;
		}
		instance_drop(&instances[i], each);
	}
}

size_t lsp_server_instance_count(void)
{
	size_t count = 0;
	size_t i;

	for (i = 0; i < LSP_SERVER_MAX_INSTANCES; i++) {
		count += instances[i].spec ? 1u : 0u;
	}
	return count;
}
