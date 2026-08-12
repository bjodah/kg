#ifndef KG_DAP_CONFIG_H
#define KG_DAP_CONFIG_H

#include "dap_transport.h" /* enum dap_transport_kind */
#include "localvars.h" /* KG_COMPILE_COMMAND_MAX */

#include <limits.h>
#include <stddef.h>

/* WHICH adapter to start and WHAT to ask it to run: the two questions a
 * debug session needs answered before it exists, and the file a user
 * answers them in.
 *
 * This module knows nothing about sessions, buffers or commands.  It reads
 * a file, validates it, and hands back records; it depends on src/json.h
 * and the C library, which is what lets a unit test link it alone.  It also
 * deliberately does NOT reach for lsp_workspace_root(): that is the
 * language server's notion of a project and it does not exist in a
 * WITH_LSP=0 build, so the debugger walks for its own marker
 * (doc/plans/dap/01-protocol.md stage 4).
 *
 * THE FILE IS TRUSTED CODE.  A launch configuration names a command to
 * spawn and arguments handed to a debugger that will run a program; there
 * is nothing to sandbox and no pretence of it, the same story as an
 * init.el.  What the validation below is for is the OTHER failure: a
 * mistyped file that would otherwise start the wrong program silently.
 * Hence a closed substitution set where an unknown `${...}` is an error
 * rather than an empty string -- `${workspaecRoot}/prog` expanding to
 * `/prog` is exactly the bug that motivated the rule.
 *
 * EVERY BOUND IS EXPLICIT, the transport's philosophy applied up the
 * stack: a file that grew a megabyte, a hundred thousand configurations or
 * an argument list that expanded without limit are all refused with a
 * message naming the file, rather than being tried.
 */

#define DAP_CONFIG_FILE_NAME ".kg-dap.json"

/* The bounds.  A configuration file is one screen of JSON in every honest
 * case; these are what an accident or a generator hits, not a user. */
#define DAP_CONFIG_MAX_FILE_BYTES (256u * 1024u)
#define DAP_CONFIG_MAX_CONFIGURATIONS 64u
#define DAP_CONFIG_MAX_ARGV 32u
/* One ordinary string: a path, a command, a name.  PATH_MAX is 4096 on the
 * platforms kg builds for, so this is "one path" rounded to a power of
 * two. */
#define DAP_CONFIG_STRING_MAX 4096u
/* Every argv element together, NUL-separated.  A bound on the whole list
 * rather than on each element alone, because 32 elements of 4 KiB each is
 * an adapter command line nobody has. */
#define DAP_CONFIG_ARGV_BYTES 4096u
/* One expanded string, and the whole expanded `arguments` value.  The
 * prototype's real launch arguments measured a few hundred bytes against
 * both v1 adapters; the room above that is for a `${env:PYTHONPATH}` or an
 * argument vector, and past it something has gone wrong rather than got
 * big. */
#define DAP_CONFIG_EXPANDED_STRING_MAX (16u * 1024u)
#define DAP_CONFIG_MAX_ARGUMENTS_BYTES (64u * 1024u)
/* How deep the `arguments` value may nest.  Well inside the JSON parser's
 * own limit, and past anything a launch configuration has ever been. */
#define DAP_CONFIG_MAX_ARGUMENTS_DEPTH 32u
/* A display name, an adapter name.  Long enough to be descriptive, short
 * enough that a chooser can show it. */
#define DAP_CONFIG_NAME_MAX 128u
/* How many directories the search for the configuration file may climb.
 * A bounded walk rather than "until /" alone, so a pathological path (a
 * symlink loop resolved into something very deep) ends. */
#define DAP_CONFIG_MAX_WALK 64u
#define DAP_CONFIG_MESSAGE_MAX 256u

/* `launch` starts a program, `attach` joins one that is already running.
 * The spec has no third value and an unknown one is refused, because a
 * misspelled request would otherwise reach an adapter as a command it does
 * not implement. */
enum dap_request_kind {
	DAP_REQUEST_LAUNCH = 0,
	DAP_REQUEST_ATTACH,
};

/* How to start one adapter.
 *
 * `cwd` is the ADAPTER's working directory and not the debuggee's -- that
 * one lives in `arguments.cwd` and is the adapter's business.  They are
 * genuinely different: delve resolves the program it is asked to debug, and
 * writes its build artefact, relative to its own directory (measured,
 * subplan 04).  So it is plumbed through kg_spawn_request.directory from
 * the start, even though neither v1 adapter needs it.
 *
 * Either `argv_bytes` (a NUL-separated list, exec'd directly) or `command`
 * (one shell command line) says what to run, never both: the built-ins and
 * inline configurations build an argv, and the environment override is a
 * shell command line for the reason KG_LSP_SERVER_<MODE> is one -- so that
 * quoting, an absolute path with spaces and a wrapper script all work
 * without this module parsing anything.
 *
 * `host`/`port` are the tcp-attach kind's, and mean nothing for the
 * others.
 *
 * `lsp_language` is the lsp-sibling kind's, and is the only field a
 * configuration file can set that names something outside the debugger: the
 * language server whose process announces this adapter beside itself
 * ("java" -> nbcode, doc/plans/dap/03-java.md).  The announce prefix and
 * the hash handshake are NOT here and are not a user's to repeat -- they
 * are the built-in spec's implementation detail, resolved through
 * src/lsp.h's endpoint query. */
struct dap_adapter_spec {
	char name[DAP_CONFIG_NAME_MAX];
	/* The `adapterID` the initialize request carries.  Adapters do
	 * report it back and dape sends the adapter's own name; kg does the
	 * same rather than impersonating an IDE. */
	char adapter_id[DAP_CONFIG_NAME_MAX];
	enum dap_transport_kind transport;
	char argv_bytes[DAP_CONFIG_ARGV_BYTES];
	size_t argv_len;
	size_t argv_count;
	char command[DAP_CONFIG_STRING_MAX];
	char cwd[PATH_MAX];
	char host[64];
	unsigned short port;
	/* The language server this adapter is a sibling of, or "" for every
	 * other kind.  A name (src/lsp_server.h's own row names), never a
	 * mode id: a configuration file cannot spell an enum. */
	char lsp_language[DAP_CONFIG_NAME_MAX];
	/* The environment variable that replaces this adapter's command
	 * line, or "" for one that has none.  A fixed shell-safe name, never
	 * derived from `name` -- a display name may contain a hyphen, and
	 * KG_DAP_ADAPTER_LLDB-DAP is not a variable anybody can set. */
	char env_override[64];
};

/* The spec's argv as a spawnable list: `out[0..n)` point into `spec`, and
 * `out[n]` is NULL, so it is what kg_spawn_request.argv takes directly.
 * Returns the element count, which is zero for a spec that carries a shell
 * command line instead. */
size_t dap_adapter_spec_argv(
    const struct dap_adapter_spec *spec, const char **out, size_t max);

/* One thing a user can ask kg to debug.
 *
 * `arguments` is the configuration's own `arguments` object, held as the
 * bytes of a JSON value: validated, re-serialised, and passed through
 * otherwise UNTOUCHED.  Members kg does not know are the point -- an
 * adapter's launch arguments are its own vocabulary, and a client that
 * filtered them to the ones it recognises is a client that cannot debug
 * anything new.  What is not passed through untouched is the substitution
 * inside its strings, which happens on a COPY at expansion time and never
 * to these bytes. */
struct dap_launch_config {
	char name[DAP_CONFIG_NAME_MAX];
	/* The name of a built-in adapter spec, or "" when the configuration
	 * spelled the adapter out inline. */
	char adapter[DAP_CONFIG_NAME_MAX];
	struct dap_adapter_spec *inline_adapter;
	enum dap_request_kind request;
	char *arguments;
	size_t arguments_len;
	/* The optional build step, run through the editor's compilation
	 * machinery (compilation_start_programmatic(), src/compile.h) before
	 * the adapter starts. */
	bool has_build;
	char build_command[KG_COMPILE_COMMAND_MAX];
	char build_cwd[PATH_MAX];
	/* Set on the built-in lldb-dap configuration and on nothing else: it
	 * has no program to debug until a user names one, and guessing
	 * `${workspaceRoot}/a.out` would be a debugger that silently debugs
	 * the wrong thing.  The command layer prompts; this flag is how it
	 * knows to. */
	bool needs_program;
	/* Whether this row came from the file or from kg's built-ins, which
	 * is what a chooser shows and what a duplicate-name rule needs. */
	bool builtin;
};

/* Everything that applies to the file under the cursor: the configurations,
 * the file they came from (empty when there was none) and the directory
 * `${workspaceRoot}` means. */
struct dap_config_set {
	char path[PATH_MAX];
	char root[PATH_MAX];
	size_t count;
	struct dap_launch_config *configs;
};

#define DAP_CONFIG_NO_OFFSET ((size_t)-1)

/* Why a configuration was refused.  Every error names the file it was in
 * and, when the refusal came from the JSON itself, the byte offset the
 * parser stopped at -- because "your configuration is wrong" without a
 * position is a message that costs the user the search. */
struct dap_config_error {
	char message[DAP_CONFIG_MESSAGE_MAX];
	char path[PATH_MAX];
	size_t offset;
};

/* Render an error as one line: `path:offset: message`, or `path: message`
 * when there is no offset, or just the message when there is no file.
 * Bounded to `out_size`, and always NUL-terminated. */
void dap_config_error_format(
    const struct dap_config_error *err, char *out, size_t out_size);

/* The built-in adapter specs: `lldb-dap` and `debugpy`, both stdio, both
 * found on PATH at run time.  Nothing is downloaded or installed, which is
 * the LSP client's rule and this one's. */
size_t dap_config_builtin_count(void);
const struct dap_adapter_spec *dap_config_builtin_at(size_t index);
const struct dap_adapter_spec *dap_config_builtin(const char *name);

/* Find the configuration file that governs `filename`: the nearest
 * `.kg-dap.json` at or above its own directory.  `root` is set to that
 * file's directory, which is what `${workspaceRoot}` means; with no file
 * anywhere above, `root` is the file's own directory and `config_path` is
 * empty.
 *
 * The walk is bounded (DAP_CONFIG_MAX_WALK) and starts from a resolved
 * path when the directory exists, so a project reached through a symlink
 * and one reached directly find the same file and produce the same root.
 * A buffer visiting no file, a special buffer, and a path whose directory
 * does not exist all fall back to the process's current directory rather
 * than failing: a user who has not saved yet is allowed to press the
 * debugger's key.
 *
 * Returns 1 when a file was found, 0 when none was, -1 when not even a
 * directory could be determined. */
int dap_config_discover(const char *filename, char *root, size_t root_size,
    char *config_path, size_t config_path_size);

/* Discovery, then the file, then kg's built-in configurations appended to
 * whatever it held.  A built-in whose display name the file already used is
 * dropped rather than duplicated -- naming a configuration `Python
 * (debugpy)` is how a user replaces kg's.
 *
 * Returns NULL with `err` filled in.  A missing file is not an error: it
 * produces the built-ins alone. */
struct dap_config_set *dap_config_load(
    const char *filename, struct dap_config_error *err);

/* The same, from bytes already in hand, with no discovery and no built-ins.
 * This is where the schema lives, and it is separate so a test can hand it
 * a document rather than a directory tree. */
struct dap_config_set *dap_config_parse(const char *text, size_t len,
    const char *config_path, const char *root, struct dap_config_error *err);

void dap_config_free(struct dap_config_set *set);

/* What the closed substitution set expands to.  Any member may be NULL,
 * and a substitution whose value is missing is an ERROR rather than an
 * empty string -- `${file}` in a buffer visiting no file is a
 * configuration that cannot run, and saying so beats launching a debugger
 * on nothing. */
struct dap_config_context {
	const char *file;
	const char *file_dir;
	const char *workspace_root;
};

/* The adapter `cfg` names, as a spec ready to spawn: the inline one it
 * carries, or the built-in it names, with the environment override applied
 * and `${...}` expanded in its own `cwd`.  `ctx` may be NULL when the spec
 * needs no expansion. */
int dap_config_resolve_adapter(const struct dap_launch_config *cfg,
    const struct dap_config_context *ctx, struct dap_adapter_spec *out,
    struct dap_config_error *err);

/* Expand one string.  The closed set is `${file}`, `${fileDir}`,
 * `${fileUri}`, `${workspaceRoot}` and `${env:NAME}`, matched longest key
 * first; a `$` that does not begin `${` is an ordinary byte; an
 * unterminated `${`, an unknown key and an unset environment variable are
 * all errors.
 *
 * `${fileUri}` is `${file}` as a `file:` URI, percent-encoded per RFC 3986
 * and refused for a path that is not absolute.  It exists because one
 * adapter parses its launch argument as a URI rather than as a path
 * (nbcode's `file`), and it is encoded inside src/dap_config.c rather than
 * through src/lsp_uri.c because that module is WITH_LSP=1 only and the
 * debugger must link without it.
 *
 * Expansion is ONE PASS: what a substitution expands TO is data and is
 * never scanned again, so an environment variable holding `${file}` is a
 * string containing those eight characters and there is no recursion to
 * bound. */
int dap_config_expand_string(const char *in,
    const struct dap_config_context *ctx, char *out, size_t out_size,
    struct dap_config_error *err);

/* Expand `cfg`'s arguments into the bytes a request carries.
 *
 * The pipeline is the point, and it is not a textual splice: the stored
 * arguments are PARSED, copied node by node with substitutions applied
 * inside decoded string values, and SERIALISED with the JSON writer.  A
 * value containing a quote, a backslash or a newline therefore arrives as
 * that value rather than as syntax; omission stays omission, null stays
 * null and a float stays a float, which matters because debugpy errors on
 * an explicit null where it expects a member to be absent.
 *
 * `*out` is a malloc'd buffer the caller frees, `*out_len` its length, and
 * together they are exactly what dap_client_request() takes as a
 * pre-encoded value. */
int dap_config_expand_arguments(const struct dap_launch_config *cfg,
    const struct dap_config_context *ctx, char **out, size_t *out_len,
    struct dap_config_error *err);

/* Build the substitution context for `filename` against `set`.  The
 * strings point into `store`, which the caller owns and which must outlive
 * the context; `file`/`file_dir` are NULL for a buffer that visits no file,
 * which is what makes `${file}` an error there rather than an empty
 * path. */
struct dap_config_context_store {
	char file[PATH_MAX];
	char file_dir[PATH_MAX];
	char root[PATH_MAX];
};

void dap_config_context_for(const char *filename,
    const struct dap_config_set *set, struct dap_config_context_store *store,
    struct dap_config_context *ctx);

#endif /* KG_DAP_CONFIG_H */
