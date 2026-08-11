#ifndef KG_LSP_SERVER_H
#define KG_LSP_SERVER_H

#include <stddef.h>

#include "syntax.h" /* enum kg_mode_id */

/* Which server a buffer's mode wants, where its workspace starts, and which
 * of them are already running.  The one module that knows the names
 * `clangd`, `ty`, `gopls`, `rust-analyzer` and `jdtls`, and the only one
 * above it that will ever need to: every caller asks for a client and gets
 * one, or gets told why not.
 *
 * Instances are keyed by (server, workspace root) and started lazily.
 * Opening a .c file spawns nothing; the first command that needs an answer
 * is what pays for the server, and a second file under the same root shares
 * it.  A server that died is not restarted where it fell: its slot is
 * emptied by the next poll that finds the client dead, and by the next
 * request for that key if one gets there first, and a fresh server is
 * started by the next command that needs one -- which is the lazy-restart
 * policy of doc/plans/2026-08-08-lsp.md.  Reclaiming on the poll is what
 * keeps servers that died in workspaces nobody revisits from holding the
 * registry full.
 *
 * syntax.h is included for `enum kg_mode_id` alone, which is why it can be:
 * that header names no editor type, so this one stays free-standing
 * (`make header-check`) and a test binary links this module without a
 * single editor object.
 */

struct lsp_client;

/* Bounds.  Four instances is two languages across two checkouts, which is
 * the working set an editing session actually has; a fifth is refused with
 * a message rather than silently making the box slower.  Sixty-four levels
 * of walk is past any real tree and short of a symlink loop's patience. */
#define LSP_SERVER_MAX_INSTANCES 4
#define LSP_SERVER_MAX_WALK 64

/* Why lsp_server_for() returned nothing.  These exist to be printed: the
 * xref stage's echo-area message is the whole reason the failures are
 * distinguished at all, since none of them is recoverable in code. */
enum lsp_server_status {
	LSP_SERVER_OK = 0,
	/* No server is configured for this mode.  Most modes; not an error. */
	LSP_SERVER_UNSUPPORTED_MODE,
	/* The path could not be resolved to a workspace root -- it was
	 * relative, empty, or too long to hold. */
	LSP_SERVER_NO_ROOT,
	LSP_SERVER_REGISTRY_FULL,
	/* fork/exec failed: no such binary on PATH, most often. */
	LSP_SERVER_SPAWN_FAILED,
};

/* The client for `mode` and the workspace `abs_path` belongs to, started if
 * it is not running yet.  `abs_path` is an absolute path to the file the
 * command was invoked on; only its directory is used, and the file need not
 * exist.
 *
 * Returns a client in any state but DEAD -- INITIALIZING is normal and is
 * not a failure: requests made against it are queued until the handshake
 * finishes (see src/lsp_client.h).  Returns NULL with `*status_out` set (a
 * NULL `status_out` is accepted) when there is no server to give.
 *
 * The client belongs to the registry.  Do not dispose of it; it is closed
 * by lsp_server_shutdown_all() or replaced when it dies. */
struct lsp_client *lsp_server_for(enum kg_mode_id mode, const char *abs_path,
    enum lsp_server_status *status_out);

/* A one-line reason for the echo area, ready to print and never NULL. */
const char *lsp_server_status_text(enum lsp_server_status status);

/* The workspace root for `mode` and `abs_path`, written to `out`.  Walks up
 * from the file's directory for the nearest ancestor holding a marker of
 * this mode's build system -- for C: `.clangd`, `compile_commands.json`,
 * `compile_flags.txt`, `build/compile_commands.json`; for Python:
 * `ty.toml`, or a `pyproject.toml` that mentions `[tool.ty` -- then, having
 * found none, for the nearest ancestor holding `.git` (a directory or the
 * file a worktree leaves), and failing that settles on the file's own
 * directory.
 *
 * Symlinks are resolved once, on the starting directory, and never again on
 * the way up: the answer is a path a server will agree with, without this
 * having an opinion about which of several routes to a directory is the
 * real one.
 *
 * Returns false only when there is no answer to give -- a relative or empty
 * path, or a root that does not fit `out_size`.  Public because the walk is
 * policy worth testing on a temporary tree without spawning anything. */
bool lsp_workspace_root(
    enum kg_mode_id mode, const char *abs_path, char *out, size_t out_size);

/* Told just before an instance's client is disposed of, whether that is a
 * dead server's slot being reclaimed or the whole registry shutting down.
 *
 * It exists so that state kept per client -- Stage 4's document shadows --
 * can be forgotten without this module knowing what that state is.  The
 * alternative, calling src/lsp_sync.h from here, would put an editor
 * dependency underneath the registry and cost every test binary that links
 * it a buffer table.  NULL, the default, means nobody is listening.
 *
 * The client is still valid when the hook runs and is gone immediately
 * afterwards, so a hook that wants to send the server anything must do it
 * there -- and Stage 4's does not, because a client being disposed of is
 * one whose server is dead or exiting. */
typedef void (*lsp_instance_drop_fn)(struct lsp_client *c);
void lsp_server_set_instance_drop_hook(lsp_instance_drop_fn fn);

/* Poll every live instance, and empty the slot of every one that has died.
 * Returns nonzero when anything happened, unchanged from lsp_client_poll(),
 * which is lsp_poll()'s repaint convention -- the reclaiming itself is not
 * something a repaint would show.  This is what src/lsp_core.c calls from
 * the editor's two poll sites. */
int lsp_server_poll_all(void);

/* Every live instance's wait descriptors, gathered into one array for the
 * editor's idle wait -- the readiness side of the poll above, and what
 * src/lsp_core.c hands up as lsp_wait_fds().  At most `max` are written;
 * KG_LSP_WAIT_FDS_MAX in src/lsp.h is the size that never truncates. */
int lsp_server_wait_fds(int *fds, int max);

/* Shut every instance down and empty the registry, for an editor that is
 * exiting.  `grace_ms` is the budget for ALL of them together, split
 * between the live ones, so the editor's exit is bounded whether one server
 * is running or four -- with a floor of a few milliseconds each, since a
 * share too small to let a server answer is not a grace period, it is a
 * formality before the kill. */
void lsp_server_shutdown_all(unsigned grace_ms);

/* How many instances the registry holds.  For tests; no policy depends on
 * it. */
size_t lsp_server_instance_count(void);

/* The environment override, spelled out because it is a user interface: for
 * every supported mode, `KG_LSP_SERVER_` followed by the mode's name in
 * upper case -- `KG_LSP_SERVER_C`, `KG_LSP_SERVER_PYTHON`,
 * `KG_LSP_SERVER_GO`, `KG_LSP_SERVER_RUST` and `KG_LSP_SERVER_JAVA` today.
 * Its value is a shell command line, run through `/bin/sh -c` exactly as M-x
 * compile's is, so quoting, an absolute path with spaces in it and a
 * wrapper script all work without this module parsing anything.  Set and
 * non-empty replaces the built-in argv; unset or empty leaves it.
 *
 * It is how every deterministic test injects test/fake_lsp_server.py, and
 * how a user points kg at a differently named or differently located
 * binary.
 *
 * One spelling means more than the command line.  A value beginning with
 * the token `listen-hash:` followed by whitespace runs the REST of it as
 * the command, and speaks to it over the socket wire instead of over its
 * standard input and output (LSP_WIRE_LISTEN_HASH, src/lsp_transport.h):
 *
 *     KG_LSP_SERVER_JAVA="listen-hash: nbcode \
 *         --start-java-language-server=listen-hash:0"
 *
 * which is Oracle's nbcode, the one server kg can start that does not
 * speak LSP on stdio.  Nothing built in selects that wire: no spec names
 * nbcode, jdt.ls stays the Java default, and the wire arrives with the
 * command line that needs it or not at all. */
#define LSP_SERVER_ENV_PREFIX "KG_LSP_SERVER_"

#endif /* KG_LSP_SERVER_H */
