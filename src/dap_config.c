/* ===================== adapter specs and launch configs =================
 *
 * See src/dap_config.h for the contract.  Stage 4 of
 * doc/plans/dap/01-protocol.md, and it links against src/json.c and the C
 * library alone -- no editor, no session, no transport code (the transport
 * header is here for one enum).
 *
 * Three things live here and they are deliberately separate: WHERE the
 * configuration comes from (a bounded upward walk for `.kg-dap.json`), WHAT
 * a valid one is (the schema and its refusals), and HOW `${...}` becomes a
 * value (a transform on decoded strings, never a splice into text).
 */

#include "dap_config.h"

#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct kg_json_value; /* src/json.h */

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* --------------------------------- errors ----------------------------- */

static void set_error(struct dap_config_error *err, const char *path,
    size_t offset, const char *fmt, ...)
{
	va_list ap;

	if (!err) {
		return;
	}
	snprintf(err->path, sizeof(err->path), "%s", path ? path : "");
	err->offset = offset;
	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	vsnprintf(err->message, sizeof(err->message), fmt, ap);
	va_end(ap);
}

void dap_config_error_format(
    const struct dap_config_error *err, char *out, size_t out_size)
{
	if (!out || out_size == 0) {
		return;
	}
	if (!err) {
		out[0] = '\0';
		return;
	}
	if (err->path[0] && err->offset != DAP_CONFIG_NO_OFFSET) {
		snprintf(out, out_size, "%s:%zu: %s", err->path, err->offset,
		    err->message);
	} else if (err->path[0]) {
		snprintf(out, out_size, "%s: %s", err->path, err->message);
	} else {
		snprintf(out, out_size, "%s", err->message);
	}
}

/* ------------------------------- built-ins ---------------------------- */

/* The adapters kg ships, found on PATH at run time like every language
 * server: kg installs nothing and downloads nothing.
 *
 * debugpy and lldb-dap run over stdio, which is what the prototype drove
 * end to end; TCP with an automatic port only becomes necessary with child
 * sessions, which v1 refuses [M-3].
 *
 * `nbcode-java` is the third and has no command of its own at all.  Its
 * adapter is a socket the Java LANGUAGE SERVER's process announces beside
 * itself, so what its row carries is the language whose server that is,
 * and starting a session means asking src/lsp.h for the endpoint rather
 * than spawning anything (doc/plans/dap/03-java.md).  It has no
 * environment override for the same reason: there is no command line to
 * replace, and the way to point kg at another nbcode is
 * KG_LSP_SERVER_JAVA. */
static const struct {
	const char *name;
	const char *adapter_id;
	const char *env;
	const char *lsp_language;
	enum dap_transport_kind transport;
	const char *cwd;
	const char *announce_prefix;
	bool route_stderr_to_output;
	const char *argv[6];
} builtin_adapter_table[] = {
	{ "lldb-dap", "lldb-dap", "KG_DAP_ADAPTER_LLDB", "",
	    DAP_TRANSPORT_STDIO, "", "", false, { "lldb-dap", NULL } },
	{ "debugpy", "debugpy", "KG_DAP_ADAPTER_DEBUGPY", "",
	    DAP_TRANSPORT_STDIO, "", "", false,
	    { "python3", "-m", "debugpy.adapter", NULL } },
	{ "nbcode-java", "nbcode-java", "", "java", DAP_TRANSPORT_LSP_SIBLING,
	    "", "", false, { NULL } },
	/* delve, and the three columns nothing before it needed.
	 *
	 * `dlv dap` is TCP-only, so kg picks the port (`127.0.0.1:0`) and
	 * scrapes the one it got -- and never passes `--log-dest`, which
	 * moves the announcement off stdout and silently defeats the scraper
	 * (measured: the first probe run timed out on exactly that).
	 *
	 * The adapter's own working directory is LOAD-BEARING here and
	 * nowhere else: delve resolves `program` and writes its `__debug_bin`
	 * artefact relative to it, and with kg's directory inherited it built
	 * the wrong module (measured).  `${workspaceRoot}` is the least
	 * surprising default in a multi-package module, where `program`
	 * selects the package; a configuration can override it.
	 *
	 * And the debuggee's stdout and stderr arrive on delve's own
	 * standard error rather than as `output` events, which is what the
	 * last column routes. */
	{ "delve", "delve", "KG_DAP_ADAPTER_DELVE", "",
	    DAP_TRANSPORT_SPAWN_PORT, "${workspaceRoot}",
	    "DAP server listening at: 127.0.0.1:", true,
	    { "dlv", "dap", "--listen", "127.0.0.1:0", NULL } },
};

/* The launch configurations that exist with no file at all.
 *
 * Python launches the file under the cursor, with the three kg-side
 * defaults the measurements forced: `internalConsole` because debugpy's own
 * default asks for a terminal through a reverse request kg does not
 * advertise and both adapters then hang [M-2]; `subProcess:false` because
 * v1 is one session [M-3]; `justMyCode:false` because a debugger that
 * silently refuses to step into the library you are debugging is worse than
 * one that steps too far.
 *
 * lldb-dap gets nothing magical, and in particular no program: it is marked
 * `needs_program`, and the command layer asks.  A `${workspaceRoot}/a.out`
 * guess would be a debugger that debugs a stale binary without saying
 * so. */
static const struct {
	const char *name;
	const char *adapter;
	const char *arguments;
	bool needs_program;
} builtin_config_table[] = {
	{ "C/C++ (lldb-dap)", "lldb-dap", "{}", true },
	{ "Python (debugpy)", "debugpy",
	    "{\"program\":\"${file}\",\"cwd\":\"${workspaceRoot}\","
	    "\"console\":\"internalConsole\",\"subProcess\":false,"
	    "\"justMyCode\":false}",
	    false },
	/* Java, and the shape is Oracle's own (debugger.ts:199-224,
	 * doc/plans/dap/03-java.md): `type` is `jdk` because nbcode's launch
	 * delegate dispatches on it, `file` is a URI and not a path because
	 * it is parsed as one, and `classPaths:["any"]` is what asks
	 * NetBeans' single-file launcher to work the classpath out.
	 * `internalConsole` is here for the reason it is on the Python row:
	 * kg does not advertise the reverse request a terminal would need. */
	{ "Java (nbcode)", "nbcode-java",
	    "{\"type\":\"jdk\",\"request\":\"launch\",\"file\":\"${fileUri}\","
	    "\"classPaths\":[\"any\"],\"console\":\"internalConsole\"}",
	    false },
	/* Go, and `mode` is the whole of it: `debug` asks delve to BUILD the
	 * package and debug what it built, which is what makes `program` a
	 * directory rather than a binary and why `${fileDir}` is the right
	 * default -- the package the file under the cursor is in.  It also
	 * passes `-gcflags="all=-N -l"` itself, which is the difference
	 * between locals you can read and a scope called
	 * `"Locals (warning: optimized function)"`; `mode:"exec"` against a
	 * plain `go build` binary is the trap, and doc/kg.1 documents the
	 * flags to build one with.
	 *
	 * `cwd` here is the DEBUGGEE's, and is deliberately not the
	 * adapter's: that one is the `delve` spec's `cwd` above. */
	{ "Go (delve)", "delve",
	    "{\"mode\":\"debug\",\"program\":\"${fileDir}\","
	    "\"cwd\":\"${fileDir}\",\"stopOnEntry\":false}",
	    false },
};

/* Append one argv element.  Both bounds are checked here rather than at the
 * call sites, so an over-long adapter command line is refused in one place
 * whether it came from the table above or from a file. */
static bool spec_push_arg(
    struct dap_adapter_spec *spec, const char *arg, size_t len)
{
	if (spec->argv_count >= DAP_CONFIG_MAX_ARGV) {
		return false;
	}
	if (spec->argv_len + len + 1 > sizeof(spec->argv_bytes)) {
		return false;
	}
	memcpy(spec->argv_bytes + spec->argv_len, arg, len);
	spec->argv_len += len;
	spec->argv_bytes[spec->argv_len++] = '\0';
	spec->argv_count++;
	return true;
}

size_t dap_adapter_spec_argv(
    const struct dap_adapter_spec *spec, const char **out, size_t max)
{
	size_t at = 0;
	size_t n = 0;

	while (n < spec->argv_count && n + 1 < max && at < spec->argv_len) {
		out[n++] = spec->argv_bytes + at;
		at += strlen(spec->argv_bytes + at) + 1;
	}
	if (max > 0) {
		out[n] = NULL;
	}
	return n;
}

static struct dap_adapter_spec
    g_builtin_specs[ARRAY_LEN(builtin_adapter_table)];
static bool g_builtins_ready;

static void builtins_init(void)
{
	size_t i;
	size_t j;

	if (g_builtins_ready) {
		return;
	}
	for (i = 0; i < ARRAY_LEN(builtin_adapter_table); i++) {
		struct dap_adapter_spec *spec = &g_builtin_specs[i];

		memset(spec, 0, sizeof(*spec));
		snprintf(spec->name, sizeof(spec->name), "%s",
		    builtin_adapter_table[i].name);
		snprintf(spec->adapter_id, sizeof(spec->adapter_id), "%s",
		    builtin_adapter_table[i].adapter_id);
		snprintf(spec->env_override, sizeof(spec->env_override), "%s",
		    builtin_adapter_table[i].env);
		snprintf(spec->lsp_language, sizeof(spec->lsp_language), "%s",
		    builtin_adapter_table[i].lsp_language);
		snprintf(spec->cwd, sizeof(spec->cwd), "%s",
		    builtin_adapter_table[i].cwd);
		snprintf(spec->announce_prefix, sizeof(spec->announce_prefix),
		    "%s", builtin_adapter_table[i].announce_prefix);
		spec->route_stderr_to_output
		    = builtin_adapter_table[i].route_stderr_to_output;
		spec->transport = builtin_adapter_table[i].transport;
		for (j = 0; builtin_adapter_table[i].argv[j]; j++) {
			(void)spec_push_arg(spec,
			    builtin_adapter_table[i].argv[j],
			    strlen(builtin_adapter_table[i].argv[j]));
		}
	}
	g_builtins_ready = true;
}

size_t dap_config_builtin_count(void) { return ARRAY_LEN(g_builtin_specs); }

const struct dap_adapter_spec *dap_config_builtin_at(size_t index)
{
	builtins_init();
	return index < ARRAY_LEN(g_builtin_specs) ? &g_builtin_specs[index]
						  : NULL;
}

const struct dap_adapter_spec *dap_config_builtin(const char *name)
{
	size_t i;

	builtins_init();
	for (i = 0; name && i < ARRAY_LEN(g_builtin_specs); i++) {
		if (strcmp(g_builtin_specs[i].name, name) == 0) {
			return &g_builtin_specs[i];
		}
	}
	return NULL;
}

/* ------------------------------ discovery ----------------------------- */

/* A buffer that visits no file at all: no name, or one of the `*special*`
 * names.  Spelled here rather than borrowed from src/bufmgr.c because this
 * module links without the editor, and the answer is one character. */
static bool visits_no_file(const char *filename)
{
	return !filename || !filename[0] || filename[0] == '*';
}

/* The directory `filename` lives in, absolute and, where the directory
 * exists, resolved.  Resolving is what makes the walk consistent: a project
 * opened through a symlink and the same project opened directly then find
 * the same `.kg-dap.json` and produce the same `${workspaceRoot}`.
 *
 * A path whose directory does not exist keeps the lexical answer, so a
 * to-be-created file in an existing project still finds its
 * configuration. */
static int directory_of_file(const char *filename, char *out, size_t size)
{
	const char *slash
	    = visits_no_file(filename) ? NULL : strrchr(filename, '/');
	char *resolved;
	char cwd[PATH_MAX];

	if (!slash) {
		return getcwd(out, size) ? 0 : -1;
	}
	if (filename[0] == '/') {
		if ((size_t)(slash - filename) >= size) {
			return -1;
		}
		memcpy(out, filename, (size_t)(slash - filename));
		out[slash - filename] = '\0';
	} else {
		if (!getcwd(cwd, sizeof(cwd))) {
			return -1;
		}
		if ((size_t)snprintf(out, size, "%s/%.*s", cwd,
			(int)(slash - filename), filename)
		    >= size) {
			return -1;
		}
	}
	if (out[0] == '\0') {
		out[0] = '/';
		out[1] = '\0';
	}
	resolved = realpath(out, NULL);
	if (resolved) {
		if (strlen(resolved) < size) {
			memcpy(out, resolved, strlen(resolved) + 1);
		}
		free(resolved);
	}
	return 0;
}

/* Climb one level, in place.  False at the root, which is where the walk
 * stops having anywhere to go. */
static bool parent_directory(char *dir)
{
	char *slash = strrchr(dir, '/');

	if (!slash || dir[1] == '\0') {
		return false;
	}
	if (slash == dir) {
		dir[1] = '\0';
		return true;
	}
	*slash = '\0';
	return true;
}

static bool is_regular_file(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int dap_config_discover(const char *filename, char *root, size_t root_size,
    char *config_path, size_t config_path_size)
{
	char dir[PATH_MAX];
	char candidate[PATH_MAX];
	unsigned step;

	if (config_path && config_path_size) {
		config_path[0] = '\0';
	}
	if (directory_of_file(filename, dir, sizeof(dir)) != 0) {
		return -1;
	}
	snprintf(root, root_size, "%s", dir);
	for (step = 0; step < DAP_CONFIG_MAX_WALK; step++) {
		int n = snprintf(candidate, sizeof(candidate), "%s/%s",
		    strcmp(dir, "/") == 0 ? "" : dir, DAP_CONFIG_FILE_NAME);

		if (n > 0 && (size_t)n < sizeof(candidate)
		    && is_regular_file(candidate)) {
			snprintf(root, root_size, "%s", dir);
			if (config_path) {
				snprintf(config_path, config_path_size, "%s",
				    candidate);
			}
			return 1;
		}
		if (!parent_directory(dir)) {
			break;
		}
	}
	return 0;
}

/* ------------------------------- the schema --------------------------- */

/* Everything a field check needs: where the error goes and what file to
 * name in it.  The offset is the parse position of the document as a whole
 * -- a validation failure is not at a byte the parser stopped at, and
 * inventing one would be a worse lie than admitting there is none. */
struct schema {
	struct dap_config_error *err;
	const char *path;
};

static bool schema_fail(struct schema *s, const char *fmt, ...)
{
	va_list ap;
	char message[DAP_CONFIG_MESSAGE_MAX];

	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	set_error(s->err, s->path, DAP_CONFIG_NO_OFFSET, "%s", message);
	return false;
}

/* One string field, with every refusal a string can earn in one place: a
 * member of the wrong type, one carrying a NUL (which would silently
 * shorten a path on its way to execvp()), and one past the bound. */
static bool field_string(struct schema *s, const struct kg_json_value *obj,
    const char *key, bool required, char *out, size_t out_size)
{
	const struct kg_json_value *v = kg_json_get(obj, key);
	size_t len = 0;
	const char *text;

	out[0] = '\0';
	if (!v) {
		return required
		    ? schema_fail(s, "a configuration has no `%s`", key)
		    : true;
	}
	text = kg_json_str(v, &len);
	if (!text) {
		return schema_fail(s, "`%s` must be a string", key);
	}
	if (len != strlen(text)) {
		return schema_fail(s, "`%s` contains a NUL byte", key);
	}
	if (len >= out_size) {
		return schema_fail(
		    s, "`%s` is longer than %zu bytes", key, out_size - 1);
	}
	memcpy(out, text, len + 1);
	return true;
}

/* The four transport kinds, and which of them have a constructor.  The
 * table is the contract; shipping status is a column in it rather than a
 * reason to leave a row out, so a configuration naming a kind this version
 * cannot do hears that rather than that it does not exist.  All four are
 * shipped today. */
static const struct {
	const char *name;
	enum dap_transport_kind kind;
	bool shipped;
} transport_table[] = {
	{ "stdio", DAP_TRANSPORT_STDIO, true },
	{ "tcp", DAP_TRANSPORT_TCP_ATTACH, true },
	{ "spawn-port", DAP_TRANSPORT_SPAWN_PORT, true },
	{ "lsp-sibling", DAP_TRANSPORT_LSP_SIBLING, true },
};

static bool parse_transport(
    struct schema *s, const char *name, enum dap_transport_kind *out)
{
	size_t i;

	if (!name[0]) {
		*out = DAP_TRANSPORT_STDIO;
		return true;
	}
	for (i = 0; i < ARRAY_LEN(transport_table); i++) {
		if (strcmp(transport_table[i].name, name) != 0) {
			continue;
		}
		if (!transport_table[i].shipped) {
			return schema_fail(s,
			    "transport `%s` is not implemented in this version",
			    name);
		}
		*out = transport_table[i].kind;
		return true;
	}
	return schema_fail(s, "unknown transport `%s`", name);
}

static bool parse_adapter_args(struct schema *s,
    const struct kg_json_value *obj, struct dap_adapter_spec *spec)
{
	const struct kg_json_value *args = kg_json_get(obj, "args");
	size_t count = kg_json_len(args);
	size_t len = 0;
	const char *text;
	size_t i;

	if (!args) {
		return true;
	}
	if (kg_json_kind_of(args) != KG_JSON_ARRAY) {
		return schema_fail(s, "an adapter's `args` must be an array");
	}
	for (i = 0; i < count; i++) {
		text = kg_json_str(kg_json_at(args, i), &len);
		if (!text || len != strlen(text)) {
			return schema_fail(s,
			    "an adapter's `args` must be strings without NUL "
			    "bytes");
		}
		if (!spec_push_arg(spec, text, len)) {
			return schema_fail(s,
			    "an adapter's command line is longer than %u "
			    "arguments or %u bytes",
			    (unsigned)DAP_CONFIG_MAX_ARGV,
			    (unsigned)DAP_CONFIG_ARGV_BYTES);
		}
	}
	return true;
}

/* An adapter spelled out in the file rather than named: `command`/`args`
 * for the kinds that spawn something, `host`/`port` for the one that
 * connects. */
static bool parse_inline_adapter(struct schema *s,
    const struct kg_json_value *obj, struct dap_adapter_spec *spec)
{
	char command[DAP_CONFIG_STRING_MAX];
	char transport[DAP_CONFIG_NAME_MAX];
	long long port;

	memset(spec, 0, sizeof(*spec));
	if (!field_string(
		s, obj, "transport", false, transport, sizeof(transport))
	    || !parse_transport(s, transport, &spec->transport)) {
		return false;
	}
	if (!field_string(s, obj, "name", false, spec->name, sizeof(spec->name))
	    || !field_string(
		s, obj, "cwd", false, spec->cwd, sizeof(spec->cwd))) {
		return false;
	}
	if (!spec->name[0]) {
		snprintf(spec->name, sizeof(spec->name), "inline");
	}
	snprintf(spec->adapter_id, sizeof(spec->adapter_id), "%s", spec->name);
	if (spec->transport == DAP_TRANSPORT_LSP_SIBLING) {
		/* No command and no address: the address is the language
		 * server's to announce, and the only thing a file has to say
		 * is WHICH language server (src/dap_config.h). */
		if (!field_string(s, obj, "lspLanguage", true,
			spec->lsp_language, sizeof(spec->lsp_language))) {
			return false;
		}
		return true;
	}
	if (spec->transport == DAP_TRANSPORT_TCP_ATTACH) {
		port = kg_json_int(kg_json_get(obj, "port"), 0);
		if (port <= 0 || port > 65535) {
			return schema_fail(s,
			    "a tcp adapter needs a `port` between 1 and 65535");
		}
		spec->port = (unsigned short)port;
		if (!field_string(s, obj, "host", false, spec->host,
			sizeof(spec->host))) {
			return false;
		}
		if (!spec->host[0]) {
			snprintf(spec->host, sizeof(spec->host), "127.0.0.1");
		}
		return true;
	}
	if (spec->transport == DAP_TRANSPORT_SPAWN_PORT
	    && !field_string(s, obj, "announcePrefix", true,
		spec->announce_prefix, sizeof(spec->announce_prefix))) {
		return false;
	}
	spec->route_stderr_to_output
	    = kg_json_bool(kg_json_get(obj, "stderrToOutput"), false);
	if (!field_string(s, obj, "command", true, command, sizeof(command))) {
		return false;
	}
	if (!spec_push_arg(spec, command, strlen(command))) {
		return schema_fail(s, "an adapter's `command` is too long");
	}
	return parse_adapter_args(s, obj, spec);
}

static bool parse_request(
    struct schema *s, const char *text, enum dap_request_kind *out)
{
	if (strcmp(text, "launch") == 0) {
		*out = DAP_REQUEST_LAUNCH;
		return true;
	}
	if (strcmp(text, "attach") == 0) {
		*out = DAP_REQUEST_ATTACH;
		return true;
	}
	return schema_fail(
	    s, "`request` must be `launch` or `attach`, not `%s`", text);
}

/* The `arguments` object, kept as the bytes of a JSON value.  Serialising
 * it here rather than retaining the parsed document is what makes a
 * configuration set self-contained -- and it proves at load time that the
 * value survives the writer, so an expansion later cannot fail for a reason
 * the file could have been refused for. */
static bool parse_arguments(struct schema *s, const struct kg_json_value *obj,
    struct dap_launch_config *cfg)
{
	const struct kg_json_value *args = kg_json_get(obj, "arguments");
	struct kg_jsonw w;

	if (!args) {
		cfg->arguments = strdup("{}");
		cfg->arguments_len = 2;
		return cfg->arguments != NULL;
	}
	if (kg_json_kind_of(args) != KG_JSON_OBJECT) {
		return schema_fail(s, "`arguments` must be an object");
	}
	kg_jsonw_init(&w);
	kg_jsonw_value(&w, args);
	if (kg_jsonw_finish(&w, &cfg->arguments, &cfg->arguments_len) != 0) {
		return schema_fail(s, "`arguments` could not be re-encoded");
	}
	if (cfg->arguments_len > DAP_CONFIG_MAX_ARGUMENTS_BYTES) {
		return schema_fail(s, "`arguments` is larger than %u bytes",
		    (unsigned)DAP_CONFIG_MAX_ARGUMENTS_BYTES);
	}
	return true;
}

static bool parse_build(struct schema *s, const struct kg_json_value *obj,
    struct dap_launch_config *cfg)
{
	const struct kg_json_value *build = kg_json_get(obj, "build");

	if (!build) {
		return true;
	}
	if (kg_json_kind_of(build) != KG_JSON_OBJECT) {
		return schema_fail(s, "`build` must be an object");
	}
	if (!field_string(s, build, "command", true, cfg->build_command,
		sizeof(cfg->build_command))
	    || !field_string(s, build, "cwd", false, cfg->build_cwd,
		sizeof(cfg->build_cwd))) {
		return false;
	}
	cfg->has_build = true;
	return true;
}

/* The adapter half of one configuration: a built-in's name, or an object
 * spelling one out.  Anything else is refused rather than defaulted --
 * "which debugger" is not a question to guess at. */
static bool parse_adapter_field(struct schema *s,
    const struct kg_json_value *obj, struct dap_launch_config *cfg)
{
	const struct kg_json_value *adapter = kg_json_get(obj, "adapter");

	if (kg_json_kind_of(adapter) == KG_JSON_OBJECT) {
		cfg->inline_adapter = calloc(1, sizeof(*cfg->inline_adapter));
		if (!cfg->inline_adapter) {
			return schema_fail(s, "out of memory");
		}
		return parse_inline_adapter(s, adapter, cfg->inline_adapter);
	}
	if (!field_string(
		s, obj, "adapter", true, cfg->adapter, sizeof(cfg->adapter))) {
		return false;
	}
	if (!dap_config_builtin(cfg->adapter)) {
		return schema_fail(s, "unknown adapter `%s`", cfg->adapter);
	}
	return true;
}

static bool parse_config(struct schema *s, const struct kg_json_value *obj,
    struct dap_launch_config *cfg)
{
	char request[DAP_CONFIG_NAME_MAX];

	if (kg_json_kind_of(obj) != KG_JSON_OBJECT) {
		return schema_fail(s, "each configuration must be an object");
	}
	if (!field_string(s, obj, "name", true, cfg->name, sizeof(cfg->name))) {
		return false;
	}
	if (!cfg->name[0]) {
		return schema_fail(s, "a configuration has an empty `name`");
	}
	if (!field_string(s, obj, "request", true, request, sizeof(request))
	    || !parse_request(s, request, &cfg->request)) {
		return false;
	}
	return parse_adapter_field(s, obj, cfg) && parse_arguments(s, obj, cfg)
	    && parse_build(s, obj, cfg);
}

/* ---------------------------- the set itself -------------------------- */

static struct dap_launch_config *set_add(struct dap_config_set *set)
{
	struct dap_launch_config *grown
	    = realloc(set->configs, (set->count + 1) * sizeof(*set->configs));

	if (!grown) {
		return NULL;
	}
	set->configs = grown;
	memset(&grown[set->count], 0, sizeof(*grown));
	return &grown[set->count++];
}

/* Whether one of the first `limit` configurations already carries `name`.
 * The limit is what lets the duplicate check ask about the entry that was
 * just filled in without finding itself. */
static bool name_taken(
    const struct dap_config_set *set, size_t limit, const char *name)
{
	size_t i;

	for (i = 0; i < limit; i++) {
		if (strcmp(set->configs[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

static bool set_has_name(const struct dap_config_set *set, const char *name)
{
	return name_taken(set, set->count, name);
}

void dap_config_free(struct dap_config_set *set)
{
	size_t i;

	if (!set) {
		return;
	}
	for (i = 0; i < set->count; i++) {
		free(set->configs[i].arguments);
		free(set->configs[i].inline_adapter);
	}
	free(set->configs);
	free(set);
}

static struct dap_config_set *set_new(const char *path, const char *root)
{
	struct dap_config_set *set = calloc(1, sizeof(*set));

	if (!set) {
		return NULL;
	}
	snprintf(set->path, sizeof(set->path), "%s", path ? path : "");
	snprintf(set->root, sizeof(set->root), "%s", root ? root : "");
	return set;
}

/* The document's own shape: a version this build knows, and an array of
 * configurations within bounds.  The version is checked first and by
 * value, so a future file says "unknown version" rather than failing on
 * whichever member changed. */
static bool parse_root(struct schema *s, const struct kg_json_value *root,
    const struct kg_json_value **configurations)
{
	const struct kg_json_value *version;

	if (kg_json_kind_of(root) != KG_JSON_OBJECT) {
		return schema_fail(
		    s, "the configuration must be a JSON object");
	}
	version = kg_json_get(root, "version");
	if (kg_json_kind_of(version) != KG_JSON_NUMBER
	    || kg_json_int(version, 0) != 1) {
		return schema_fail(
		    s, "unknown configuration `version` (this kg reads 1)");
	}
	*configurations = kg_json_get(root, "configurations");
	if (kg_json_kind_of(*configurations) != KG_JSON_ARRAY) {
		return schema_fail(s, "`configurations` must be an array");
	}
	if (kg_json_len(*configurations) > DAP_CONFIG_MAX_CONFIGURATIONS) {
		return schema_fail(s, "more than %u configurations",
		    (unsigned)DAP_CONFIG_MAX_CONFIGURATIONS);
	}
	return true;
}

static bool parse_configurations(struct schema *s, struct dap_config_set *set,
    const struct kg_json_value *configurations)
{
	struct dap_launch_config *cfg;
	size_t i;

	for (i = 0; i < kg_json_len(configurations); i++) {
		cfg = set_add(set);
		if (!cfg) {
			return schema_fail(s, "out of memory");
		}
		if (!parse_config(s, kg_json_at(configurations, i), cfg)) {
			return false;
		}
		/* Two configurations with one name is a file whose chooser
		 * would show the same row twice and whose "last used" could
		 * not name either. */
		if (name_taken(set, set->count - 1, cfg->name)) {
			return schema_fail(
			    s, "two configurations named `%s`", cfg->name);
		}
	}
	return true;
}

struct dap_config_set *dap_config_parse(const char *text, size_t len,
    const char *config_path, const char *root, struct dap_config_error *err)
{
	struct schema s = { err, config_path };
	const struct kg_json_value *configurations = NULL;
	struct dap_config_set *set;
	struct kg_json *doc;
	size_t offset = 0;

	if (len > DAP_CONFIG_MAX_FILE_BYTES) {
		set_error(err, config_path, DAP_CONFIG_NO_OFFSET,
		    "the configuration file is larger than %u bytes",
		    (unsigned)DAP_CONFIG_MAX_FILE_BYTES);
		return NULL;
	}
	/* Duplicate members are refused here rather than resolved silently:
	 * a file naming one setting twice has two answers and no way to say
	 * which the user meant. */
	doc = kg_json_parse_ex(
	    text, len, KG_JSON_REJECT_DUPLICATE_KEYS, &offset);
	if (!doc) {
		set_error(err, config_path, offset,
		    "the configuration is not valid JSON, names one member "
		    "twice, or nests too deeply");
		return NULL;
	}
	set = set_new(config_path, root);
	if (!set) {
		kg_json_free(doc);
		set_error(
		    err, config_path, DAP_CONFIG_NO_OFFSET, "out of memory");
		return NULL;
	}
	if (!parse_root(&s, kg_json_root(doc), &configurations)
	    || !parse_configurations(&s, set, configurations)) {
		kg_json_free(doc);
		dap_config_free(set);
		return NULL;
	}
	kg_json_free(doc);
	return set;
}

/* kg's own configurations, appended to whatever the file held.  A built-in
 * whose display name the file already used is dropped: naming a
 * configuration `Python (debugpy)` is how a user replaces kg's, and two
 * rows with one name is what the file itself is refused for. */
static bool append_builtins(struct dap_config_set *set)
{
	struct dap_launch_config *cfg;
	size_t i;

	for (i = 0; i < ARRAY_LEN(builtin_config_table); i++) {
		if (set_has_name(set, builtin_config_table[i].name)) {
			continue;
		}
		cfg = set_add(set);
		if (!cfg) {
			return false;
		}
		snprintf(cfg->name, sizeof(cfg->name), "%s",
		    builtin_config_table[i].name);
		snprintf(cfg->adapter, sizeof(cfg->adapter), "%s",
		    builtin_config_table[i].adapter);
		cfg->request = DAP_REQUEST_LAUNCH;
		cfg->needs_program = builtin_config_table[i].needs_program;
		cfg->builtin = true;
		cfg->arguments = strdup(builtin_config_table[i].arguments);
		if (!cfg->arguments) {
			return false;
		}
		cfg->arguments_len = strlen(cfg->arguments);
	}
	return true;
}

/* Read the whole file, bounded.  A file larger than the bound is refused
 * without being read into memory first, which is the point of asking its
 * size. */
static char *read_file(
    const char *path, size_t *len, struct dap_config_error *err)
{
	FILE *fp = fopen(path, "rb");
	struct stat st;
	char *text;
	size_t got;

	if (!fp) {
		set_error(err, path, DAP_CONFIG_NO_OFFSET,
		    "the configuration file could not be opened");
		return NULL;
	}
	if (fstat(fileno(fp), &st) != 0
	    || (size_t)st.st_size > DAP_CONFIG_MAX_FILE_BYTES) {
		fclose(fp);
		set_error(err, path, DAP_CONFIG_NO_OFFSET,
		    "the configuration file is larger than %u bytes",
		    (unsigned)DAP_CONFIG_MAX_FILE_BYTES);
		return NULL;
	}
	text = malloc((size_t)st.st_size + 1);
	if (!text) {
		fclose(fp);
		set_error(err, path, DAP_CONFIG_NO_OFFSET, "out of memory");
		return NULL;
	}
	got = fread(text, 1, (size_t)st.st_size, fp);
	fclose(fp);
	/* fread returns at most the count it was asked for, and the buffer
	 * is one byte longer; the ArrayBound checker does not model the
	 * first half of that. */
	// NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
	text[got] = '\0';
	*len = got;
	return text;
}

struct dap_config_set *dap_config_load(
    const char *filename, struct dap_config_error *err)
{
	char root[PATH_MAX];
	char path[PATH_MAX];
	struct dap_config_set *set;
	char *text;
	size_t len = 0;
	int found;

	if (err) {
		memset(err, 0, sizeof(*err));
		err->offset = DAP_CONFIG_NO_OFFSET;
	}
	found = dap_config_discover(
	    filename, root, sizeof(root), path, sizeof(path));
	if (found < 0) {
		set_error(err, "", DAP_CONFIG_NO_OFFSET,
		    "the current directory could not be determined");
		return NULL;
	}
	if (found == 0) {
		set = set_new("", root);
		if (!set) {
			set_error(
			    err, "", DAP_CONFIG_NO_OFFSET, "out of memory");
		}
	} else {
		text = read_file(path, &len, err);
		if (!text) {
			return NULL;
		}
		set = dap_config_parse(text, len, path, root, err);
		free(text);
	}
	if (!set) {
		return NULL;
	}
	if (!append_builtins(set)) {
		set_error(err, path, DAP_CONFIG_NO_OFFSET, "out of memory");
		dap_config_free(set);
		return NULL;
	}
	return set;
}

void dap_config_context_for(const char *filename,
    const struct dap_config_set *set, struct dap_config_context_store *store,
    struct dap_config_context *ctx)
{
	memset(store, 0, sizeof(*store));
	memset(ctx, 0, sizeof(*ctx));
	if (set) {
		snprintf(store->root, sizeof(store->root), "%s", set->root);
		ctx->workspace_root = store->root;
	}
	if (visits_no_file(filename)) {
		return;
	}
	snprintf(store->file, sizeof(store->file), "%s", filename);
	ctx->file = store->file;
	if (directory_of_file(
		filename, store->file_dir, sizeof(store->file_dir))
	    == 0) {
		ctx->file_dir = store->file_dir;
	}
}

/* ----------------------------- substitution --------------------------- */

/* The closed set, longest key first so that `${fileDir}` can never be read
 * as `${file}` with a stray `Dir}` after it.  (The scan takes the whole key
 * up to its `}` and compares lengths too, so the order is belt and braces
 * -- but it is the order the rule is stated in, and the next key added
 * should keep it.)
 *
 * The `env:` family is tested first because it is the only key with an
 * argument; the fixed keys follow, longest first.
 *
 * An unknown key is an ERROR and an unset `${env:NAME}` is an error, both
 * for the same reason: the alternative is an empty string, and
 * `${workspaecRoot}/prog` silently becoming `/prog` launches the wrong
 * program with no diagnosis.  A default syntax (`${env:NAME:-fallback}`)
 * would be the way to allow one, and it does not exist yet. */
/* `${env:NAME}`, the one key that carries an argument.  Its own function
 * because the closed set below stays a list of comparisons that way, and
 * because an unset variable is an error with a name in it. */
static const char *resolve_env(const char *name, size_t len, char *scratch,
    size_t scratch_size, const char **why)
{
	const char *value;

	if (len == 0 || len >= scratch_size) {
		*why = "does not name an environment variable kg can read";
		return NULL;
	}
	memcpy(scratch, name, len);
	scratch[len] = '\0';
	value = getenv(scratch);
	*why = value ? NULL : "names an environment variable that is not set";
	return value;
}

/* `${fileUri}`: the file under the cursor as a `file:` URI, because one
 * adapter parses its launch argument as a URI rather than as a path
 * (nbcode's `file`, debugger.ts:199-224 -- `new URI(s)`, doc/plans/dap/
 * 03-java.md).
 *
 * Encoded HERE rather than through src/lsp_uri.c, and that is deliberate
 * rather than lazy.  lsp_uri.c is compiled only in a WITH_LSP=1 build, and
 * a debugger that linked it would stop linking in the configuration
 * .ci/ci-14 builds -- the one where the Java adapter's whole job is to say
 * it is not available.  The rule the two share is RFC 3986's unreserved
 * set plus `/`, so this is that rule written twice, in two modules that
 * must not depend on each other, with a test each.
 *
 * A buffer that has not been saved has no file for an adapter to open and
 * is refused.  A path that is merely RELATIVE is made absolute first,
 * against kg's own working directory, which is exactly what such a path
 * means: `kg Fixture.java` visits a real saved file, and a URI is not
 * allowed to be relative -- leaving it so would hand the adapter something
 * it resolves against ITS directory, which is not where the user is. */
static bool uri_unreserved(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
	    || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
	    || c == '~' || c == '/';
}

/* `path`, made absolute against kg's own working directory when it is not
 * already, which is exactly what a relative buffer filename means. */
static bool file_uri_absolute(const char *path, char *out, size_t out_size)
{
	char cwd[PATH_MAX];
	int wrote;

	if (path[0] == '/') {
		return (size_t)snprintf(out, out_size, "%s", path) < out_size;
	}
	if (!getcwd(cwd, sizeof(cwd))) {
		return false;
	}
	wrote = snprintf(out, out_size, "%s/%s", cwd, path);
	return wrote >= 0 && (size_t)wrote < out_size;
}

static const char *resolve_file_uri(
    const char *path, char *scratch, size_t scratch_size, const char **why)
{
	static const char hex[] = "0123456789ABCDEF";
	char absolute[PATH_MAX];
	size_t n = 7;
	size_t i;

	if (!path || !path[0]) {
		*why = "has no value here (the buffer visits no file)";
		return NULL;
	}
	if (!file_uri_absolute(path, absolute, sizeof(absolute))) {
		*why = "cannot be made into an absolute path";
		return NULL;
	}
	path = absolute;
	if (scratch_size < 9) {
		*why = "does not fit";
		return NULL;
	}
	memcpy(scratch, "file://", 7);
	for (i = 0; path[i]; i++) {
		unsigned char c = (unsigned char)path[i];
		bool plain = uri_unreserved(c);

		if (n + (plain ? 1u : 3u) >= scratch_size) {
			*why = "does not fit";
			return NULL;
		}
		if (plain) {
			scratch[n++] = (char)c;
			continue;
		}
		scratch[n++] = '%';
		scratch[n++] = hex[c >> 4];
		scratch[n++] = hex[c & 0x0f];
	}
	scratch[n] = '\0';
	return scratch;
}

static const char *resolve_key(const struct dap_config_context *ctx,
    const char *key, size_t key_len, char *scratch, size_t scratch_size,
    const char **why)
{
	static const struct dap_config_context nothing;
	const char *value;

	*why = NULL;
	if (!ctx) {
		ctx = &nothing;
	}
	if (key_len > 4 && memcmp(key, "env:", 4) == 0) {
		return resolve_env(
		    key + 4, key_len - 4, scratch, scratch_size, why);
	}
	if (key_len == 13 && memcmp(key, "workspaceRoot", 13) == 0) {
		value = ctx->workspace_root;
	} else if (key_len == 7 && memcmp(key, "fileDir", 7) == 0) {
		value = ctx->file_dir;
	} else if (key_len == 7 && memcmp(key, "fileUri", 7) == 0) {
		if (!ctx->file || !ctx->file[0]) {
			*why = "has no value here (the buffer visits no file)";
			return NULL;
		}
		return resolve_file_uri(ctx->file, scratch, scratch_size, why);
	} else if (key_len == 4 && memcmp(key, "file", 4) == 0) {
		value = ctx->file;
	} else {
		*why = "is not one of ${file}, ${fileDir}, ${fileUri}, "
		       "${workspaceRoot} or ${env:NAME}";
		return NULL;
	}
	if (!value || !value[0]) {
		*why = "has no value here (the buffer visits no file)";
		return NULL;
	}
	return value;
}

static bool append_bytes(
    char *out, size_t out_size, size_t *used, const char *text, size_t len)
{
	if (*used + len + 1 > out_size) {
		return false;
	}
	memcpy(out + *used, text, len);
	*used += len;
	out[*used] = '\0';
	return true;
}

int dap_config_expand_string(const char *in,
    const struct dap_config_context *ctx, char *out, size_t out_size,
    struct dap_config_error *err)
{
	char scratch[DAP_CONFIG_NAME_MAX];
	const char *p = in;
	const char *why;
	const char *value;
	const char *close;
	size_t used = 0;

	if (out_size == 0) {
		return -1;
	}
	out[0] = '\0';
	while (*p) {
		if (p[0] != '$' || p[1] != '{') {
			if (!append_bytes(out, out_size, &used, p, 1)) {
				break;
			}
			p++;
			continue;
		}
		close = strchr(p + 2, '}');
		if (!close) {
			set_error(err, "", DAP_CONFIG_NO_OFFSET,
			    "a `${` in the configuration is never closed");
			return -1;
		}
		value = resolve_key(ctx, p + 2, (size_t)(close - (p + 2)),
		    scratch, sizeof(scratch), &why);
		if (!value) {
			set_error(err, "", DAP_CONFIG_NO_OFFSET, "`%.*s` %s",
			    (int)(close + 1 - p), p, why);
			return -1;
		}
		/* One pass: what a substitution expands TO is data, and is
		 * never scanned for substitutions of its own. */
		if (!append_bytes(out, out_size, &used, value, strlen(value))) {
			break;
		}
		p = close + 1;
	}
	if (*p) {
		set_error(err, "", DAP_CONFIG_NO_OFFSET,
		    "an expanded value is longer than %zu bytes", out_size - 1);
		return -1;
	}
	return 0;
}

/* --------------------- substitution inside `arguments` ---------------- */

/* The recursive copy's state.  `scratch` is one buffer rather than one per
 * frame because strings are leaves: a string is expanded and written before
 * anything else can need the buffer. */
struct arg_copy {
	const struct dap_config_context *ctx;
	struct dap_config_error *err;
	char *scratch;
	bool failed;
};

static void copy_string(
    struct arg_copy *ac, const struct kg_json_value *v, struct kg_jsonw *w)
{
	size_t len = 0;
	const char *text = kg_json_str(v, &len);

	if (!text || len != strlen(text)) {
		set_error(ac->err, "", DAP_CONFIG_NO_OFFSET,
		    "an argument string contains a NUL byte");
		ac->failed = true;
		return;
	}
	if (dap_config_expand_string(text, ac->ctx, ac->scratch,
		DAP_CONFIG_EXPANDED_STRING_MAX, ac->err)
	    != 0) {
		ac->failed = true;
		return;
	}
	kg_jsonw_stringn(w, ac->scratch, strlen(ac->scratch));
}

/* A copy, not a splice.  Every kind is written through the writer, so a
 * value containing a quote or a backslash arrives as that value; null stays
 * null rather than becoming an omission; and a number goes through
 * kg_jsonw_number(), so a float stays a float. */
static void copy_value(struct arg_copy *ac, const struct kg_json_value *v,
    struct kg_jsonw *w, unsigned depth)
{
	size_t count = kg_json_len(v);
	const char *key;
	size_t key_len = 0;
	size_t i;

	if (depth > DAP_CONFIG_MAX_ARGUMENTS_DEPTH || ac->failed) {
		ac->failed = true;
		return;
	}
	switch (kg_json_kind_of(v)) {
	case KG_JSON_STRING:
		copy_string(ac, v, w);
		return;
	case KG_JSON_ARRAY:
		kg_jsonw_begin_array(w);
		for (i = 0; i < count; i++) {
			copy_value(ac, kg_json_at(v, i), w, depth + 1);
		}
		kg_jsonw_end_array(w);
		return;
	case KG_JSON_OBJECT:
		kg_jsonw_begin_object(w);
		for (i = 0; i < count; i++) {
			key = kg_json_key_at(v, i, &key_len);
			/* Keys are copied, never expanded: a substitution
			 * belongs in a value, and an adapter's argument names
			 * are its own vocabulary. */
			kg_jsonw_keyn(w, key ? key : "", key ? key_len : 0);
			copy_value(ac, kg_json_at(v, i), w, depth + 1);
		}
		kg_jsonw_end_object(w);
		return;
	default:
		kg_jsonw_value(w, v);
		return;
	}
}

int dap_config_expand_arguments(const struct dap_launch_config *cfg,
    const struct dap_config_context *ctx, char **out, size_t *out_len,
    struct dap_config_error *err)
{
	struct arg_copy ac = { ctx, err, NULL, false };
	struct kg_jsonw w;
	struct kg_json *doc;

	*out = NULL;
	*out_len = 0;
	doc = kg_json_parse(cfg->arguments, cfg->arguments_len, NULL);
	ac.scratch = malloc(DAP_CONFIG_EXPANDED_STRING_MAX);
	if (!doc || !ac.scratch) {
		free(ac.scratch);
		kg_json_free(doc);
		set_error(err, "", DAP_CONFIG_NO_OFFSET,
		    "the configuration's `arguments` could not be read back");
		return -1;
	}
	kg_jsonw_init(&w);
	copy_value(&ac, kg_json_root(doc), &w, 0);
	free(ac.scratch);
	kg_json_free(doc);
	if (ac.failed || kg_jsonw_finish(&w, out, out_len) != 0) {
		kg_jsonw_free(&w);
		free(*out);
		*out = NULL;
		*out_len = 0;
		if (!err || !err->message[0]) {
			set_error(err, "", DAP_CONFIG_NO_OFFSET,
			    "the configuration's `arguments` could not be "
			    "expanded");
		}
		return -1;
	}
	if (*out_len > DAP_CONFIG_MAX_ARGUMENTS_BYTES) {
		free(*out);
		*out = NULL;
		*out_len = 0;
		set_error(err, "", DAP_CONFIG_NO_OFFSET,
		    "the expanded `arguments` are larger than %u bytes",
		    (unsigned)DAP_CONFIG_MAX_ARGUMENTS_BYTES);
		return -1;
	}
	return 0;
}

/* --------------------------- resolving an adapter --------------------- */

/* The environment override, per adapter and with a fixed shell-safe name:
 * `KG_DAP_ADAPTER_LLDB` and `KG_DAP_ADAPTER_DEBUGPY`, mirroring
 * KG_LSP_SERVER_<MODE> (src/lsp_server.h).  Its value is a shell command
 * line, run through /bin/sh -c exactly as M-x compile's is, so quoting, an
 * absolute path with spaces and a wrapper script all work without this
 * module parsing anything.  Set and non-empty replaces the built-in argv;
 * unset or empty leaves it. */
static void apply_env_override(struct dap_adapter_spec *spec)
{
	const char *value;

	if (!spec->env_override[0]) {
		return;
	}
	value = getenv(spec->env_override);
	if (!value || !value[0] || strlen(value) >= sizeof(spec->command)) {
		return;
	}
	snprintf(spec->command, sizeof(spec->command), "%s", value);
	spec->argv_count = 0;
	spec->argv_len = 0;
}

int dap_config_resolve_adapter(const struct dap_launch_config *cfg,
    const struct dap_config_context *ctx, struct dap_adapter_spec *out,
    struct dap_config_error *err)
{
	const struct dap_adapter_spec *base = cfg->inline_adapter;
	char expanded[PATH_MAX];

	if (!base) {
		base = dap_config_builtin(cfg->adapter);
	}
	if (!base) {
		set_error(err, "", DAP_CONFIG_NO_OFFSET, "unknown adapter `%s`",
		    cfg->adapter);
		return -1;
	}
	*out = *base;
	apply_env_override(out);
	if (out->cwd[0]) {
		if (dap_config_expand_string(
			out->cwd, ctx, expanded, sizeof(expanded), err)
		    != 0) {
			return -1;
		}
		snprintf(out->cwd, sizeof(out->cwd), "%s", expanded);
	}
	return 0;
}
