#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp, setenv, symlink under -std=c23 */
#endif

/* test_dap_config.c -- adapter specs, `.kg-dap.json` and the substitution
 * pipeline (src/dap_config.c, stage 4 of doc/plans/dap/01-protocol.md).
 *
 * Three kinds of case, and they fail differently, which is why they are
 * kept apart here:
 *
 *   - the SCHEMA: every refusal the plan names, each asserted to be a
 *     refusal rather than a silently accepted document, and each asserted
 *     to name the file it was in.
 *   - DISCOVERY: a real directory tree under mkdtemp, walked upward,
 *     including the two edges a walk gets wrong -- a buffer visiting no
 *     file, and a path that does not exist -- and the symlink question,
 *     since a project reached two ways must produce one ${workspaceRoot}.
 *   - SUBSTITUTION: the transform is on decoded strings, so the cases that
 *     matter are the ones a textual splice would break -- a quote, a
 *     backslash, a newline, a multi-byte character -- and they are asserted
 *     by PARSING the result back, because the property is "this is still
 *     the same value", not "these are the bytes I expected".
 */

#include "../src/dap_config.h"
#include "../src/json.h"
#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------- helpers ------------------------------ */

static struct dap_config_error last_error;

static struct dap_config_set *parse(const char *text)
{
	memset(&last_error, 0, sizeof(last_error));
	last_error.offset = DAP_CONFIG_NO_OFFSET;
	return dap_config_parse(
	    text, strlen(text), "/p/.kg-dap.json", "/p", &last_error);
}

/* A document that must be refused, with the message naming the file: a
 * refusal a user cannot locate is half a refusal. */
static void refused(const char *text, const char *expect)
{
	struct dap_config_set *set = parse(text);
	char rendered[512];

	CHECKF(set == NULL, "accepted: %s", text);
	dap_config_free(set);
	dap_config_error_format(&last_error, rendered, sizeof(rendered));
	CHECKF(strstr(rendered, "/p/.kg-dap.json") != NULL, "no path in `%s`",
	    rendered);
	CHECKF(strstr(rendered, expect) != NULL, "`%s` lacks `%s`", rendered,
	    expect);
}

static bool write_file(const char *path, const char *text)
{
	FILE *fp = fopen(path, "w");

	if (!fp) {
		return false;
	}
	fputs(text, fp);
	return fclose(fp) == 0;
}

/* Expand `in` and hand back the expansion, or NULL with `last_error` set. */
static const char *expand(const char *in, const struct dap_config_context *ctx)
{
	static char out[8192];

	memset(&last_error, 0, sizeof(last_error));
	last_error.offset = DAP_CONFIG_NO_OFFSET;
	if (dap_config_expand_string(in, ctx, out, sizeof(out), &last_error)
	    != 0) {
		return NULL;
	}
	return out;
}

/* The expansion succeeded AND said `want`.  expand() may return NULL, so
 * no CHECK hands its result to strcmp directly -- a failed expansion must
 * fail the case, not crash it. */
static bool expands_to(
    const char *in, const struct dap_config_context *ctx, const char *want)
{
	const char *got = expand(in, ctx);

	return got != NULL && strcmp(got, want) == 0;
}

/* The value of `key` in an expanded arguments document, as a string. */
static bool expanded_member(const char *arguments,
    const struct dap_config_context *ctx, const char *key, char *out,
    size_t out_size)
{
	struct dap_launch_config cfg = { 0 };
	const struct kg_json_value *member;
	struct kg_json *doc;
	char *bytes = NULL;
	size_t len = 0;
	const char *text;
	bool ok = false;

	cfg.arguments = (char *)(uintptr_t)arguments;
	cfg.arguments_len = strlen(arguments);
	memset(&last_error, 0, sizeof(last_error));
	last_error.offset = DAP_CONFIG_NO_OFFSET;
	if (dap_config_expand_arguments(&cfg, ctx, &bytes, &len, &last_error)
	    != 0) {
		return false;
	}
	doc = kg_json_parse(bytes, len, NULL);
	member = kg_json_get(kg_json_root(doc), key);
	text = kg_json_str(member, NULL);
	if (text && strlen(text) < out_size) {
		memcpy(out, text, strlen(text) + 1);
		ok = true;
	}
	kg_json_free(doc);
	free(bytes);
	return ok;
}

/* ------------------------------- built-ins ---------------------------- */

/* The two adapters that exist with no file at all, and the kg-side
 * defaults baked into their configurations: debugpy's three [M-2, M-3],
 * and lldb-dap's deliberate lack of one. */
static void test_builtin_adapters_and_their_defaults(void)
{
	const struct dap_adapter_spec *lldb = dap_config_builtin("lldb-dap");
	const struct dap_adapter_spec *debugpy = dap_config_builtin("debugpy");
	const char *argv[DAP_CONFIG_MAX_ARGV + 1];

	CHECK(dap_config_builtin_count() == 3);
	CHECK(lldb != NULL && debugpy != NULL);
	CHECK(dap_config_builtin("nosuchadapter") == NULL);
	if (!lldb || !debugpy) {
		return;
	}
	CHECK(lldb->transport == DAP_TRANSPORT_STDIO);
	CHECK(strcmp(lldb->adapter_id, "lldb-dap") == 0);
	CHECK(strcmp(lldb->env_override, "KG_DAP_ADAPTER_LLDB") == 0);
	CHECK(dap_adapter_spec_argv(lldb, argv, DAP_CONFIG_MAX_ARGV + 1) == 1);
	CHECK(strcmp(argv[0], "lldb-dap") == 0);
	CHECK(argv[1] == NULL);

	CHECK(strcmp(debugpy->env_override, "KG_DAP_ADAPTER_DEBUGPY") == 0);
	CHECK(
	    dap_adapter_spec_argv(debugpy, argv, DAP_CONFIG_MAX_ARGV + 1) == 3);
	CHECK(strcmp(argv[0], "python3") == 0);
	CHECK(strcmp(argv[1], "-m") == 0);
	CHECK(strcmp(argv[2], "debugpy.adapter") == 0);
}

static void test_builtin_configs_when_no_file_exists(void)
{
	char template[] = "/tmp/kg-dapcfg-XXXXXX";
	char source[PATH_MAX];
	struct dap_config_set *set;
	size_t i;
	size_t python = DAP_CONFIG_MAX_CONFIGURATIONS;
	size_t lldb = DAP_CONFIG_MAX_CONFIGURATIONS;

	CHECK(mkdtemp(template) != NULL);
	snprintf(source, sizeof(source), "%s/main.py", template);
	CHECK(write_file(source, "print(1)\n"));
	set = dap_config_load(source, &last_error);
	CHECK(set != NULL);
	if (!set) {
		return;
	}
	CHECK(set->path[0] == '\0');
	CHECK(strstr(set->root, template + 5) != NULL);
	CHECK(set->count == 3);
	for (i = 0; i < set->count; i++) {
		CHECK(set->configs[i].builtin);
		if (strcmp(set->configs[i].adapter, "debugpy") == 0) {
			python = i;
		}
		if (strcmp(set->configs[i].adapter, "lldb-dap") == 0) {
			lldb = i;
		}
	}
	CHECK(python < set->count && lldb < set->count);
	if (python < set->count) {
		const char *args = set->configs[python].arguments;

		CHECK(strstr(args, "\"program\":\"${file}\"") != NULL);
		CHECK(strstr(args, "\"console\":\"internalConsole\"") != NULL);
		CHECK(strstr(args, "\"subProcess\":false") != NULL);
		CHECK(strstr(args, "\"justMyCode\":false") != NULL);
		CHECK(!set->configs[python].needs_program);
	}
	if (lldb < set->count) {
		/* Nothing magical, and above all no `a.out` guess: the
		 * program is a question, and the flag is how it gets
		 * asked. */
		CHECK(strcmp(set->configs[lldb].arguments, "{}") == 0);
		CHECK(set->configs[lldb].needs_program);
	}
	dap_config_free(set);
	unlink(source);
	rmdir(template);
}

/* --------------------------------- schema ----------------------------- */

static void test_a_minimal_configuration_is_accepted(void)
{
	struct dap_config_set *set
	    = parse("{\"version\":1,\"configurations\":["
		    "{\"name\":\"run\",\"adapter\":"
		    "\"debugpy\",\"request\":\"launch\","
		    "\"arguments\":{\"program\":\"x.py\"}"
		    "}]}");

	CHECK(set != NULL);
	if (!set) {
		return;
	}
	CHECK(set->count == 1);
	CHECK(strcmp(set->configs[0].name, "run") == 0);
	CHECK(set->configs[0].request == DAP_REQUEST_LAUNCH);
	CHECK(!set->configs[0].builtin);
	CHECK(strcmp(set->configs[0].arguments, "{\"program\":\"x.py\"}") == 0);
	dap_config_free(set);
}

/* Every rejection the plan names, in one case each, all asserting the
 * message names the file. */
static void test_the_schema_refuses_what_it_says_it_does(void)
{
	refused("{\"version\":2,\"configurations\":[]}", "version");
	refused("[]", "object");
	refused("{\"version\":1}", "configurations");
	refused(
	    "{\"version\":1,\"configurations\":["
	    "{\"name\":\"a\",\"adapter\":\"debugpy\",\"request\":\"walk\"}]}",
	    "request");
	refused("{\"version\":1,\"configurations\":["
		"{\"adapter\":\"debugpy\",\"request\":\"launch\"}]}",
	    "name");
	refused(
	    "{\"version\":1,\"configurations\":["
	    "{\"name\":\"a\",\"adapter\":\"nope\",\"request\":\"launch\"}]}",
	    "unknown adapter");
	refused(
	    "{\"version\":1,\"configurations\":["
	    "{\"name\":\"a\",\"adapter\":\"debugpy\",\"request\":\"launch\"},"
	    "{\"name\":\"a\",\"adapter\":\"debugpy\",\"request\":\"attach\"}]}",
	    "two configurations named");
	/* A NUL inside a JSON string is legal JSON and would silently
	 * shorten a path on its way to execvp(). */
	refused("{\"version\":1,\"configurations\":["
		"{\"name\":\"a\\u0000b\",\"adapter\":\"debugpy\","
		"\"request\":\"launch\"}]}",
	    "NUL");
	refused("{\"version\":1,\"configurations\":["
		"{\"name\":\"a\",\"request\":\"launch\",\"adapter\":"
		"{\"command\":\"x\",\"transport\":\"carrier-pigeon\"}}]}",
	    "unknown transport");
	/* The one kind the enum carries and this version cannot build.
	 * `lsp-sibling` used to be the second; it ships now, and what it
	 * refuses instead is an adapter that names no language server
	 * (test_dap_java.c has that case and the accepting one). */
	refused("{\"version\":1,\"configurations\":["
		"{\"name\":\"a\",\"request\":\"launch\",\"adapter\":"
		"{\"command\":\"x\",\"transport\":\"spawn-port\"}}]}",
	    "not implemented");
	refused(
	    "{\"version\":1,\"configurations\":["
	    "{\"name\":\"a\",\"adapter\":\"debugpy\",\"request\":\"launch\","
	    "\"arguments\":[]}]}",
	    "arguments");
	refused(
	    "{\"version\":1,\"configurations\":["
	    "{\"name\":\"a\",\"adapter\":\"debugpy\",\"request\":\"launch\","
	    "\"build\":{}}]}",
	    "command");
}

/* Two spellings of one member is a document with two answers and no way to
 * say which was meant.  The JSON layer refuses it under the flag the
 * configuration reader passes and nobody else does, and the refusal carries
 * the parse offset. */
static void test_duplicate_json_keys_are_refused_with_an_offset(void)
{
	const char *text
	    = "{\"version\":1,\"version\":1,\"configurations\":[]}";
	struct dap_config_set *set = parse(text);
	char rendered[512];

	CHECK(set == NULL);
	dap_config_free(set);
	CHECK(last_error.offset != DAP_CONFIG_NO_OFFSET);
	dap_config_error_format(&last_error, rendered, sizeof(rendered));
	CHECK(strstr(rendered, "/p/.kg-dap.json:") != NULL);
	/* The same document is fine for a protocol message, which is why
	 * the flag exists rather than the parser changing under everyone. */
	{
		struct kg_json *doc = kg_json_parse(text, strlen(text), NULL);

		CHECK(doc != NULL);
		kg_json_free(doc);
	}
}

static void test_the_bounds_are_real(void)
{
	char big[DAP_CONFIG_MAX_FILE_BYTES + 64];
	char many[8192];
	char args[4096];
	size_t used;
	size_t i;

	memset(big, ' ', sizeof(big));
	big[sizeof(big) - 1] = '\0';
	memset(&last_error, 0, sizeof(last_error));
	CHECK(dap_config_parse(
		  big, sizeof(big) - 1, "/p/.kg-dap.json", "/p", &last_error)
	    == NULL);
	CHECK(strstr(last_error.message, "larger than") != NULL);

	used = (size_t)snprintf(
	    many, sizeof(many), "%s", "{\"version\":1,\"configurations\":[");
	for (i = 0; i <= DAP_CONFIG_MAX_CONFIGURATIONS; i++) {
		used += (size_t)snprintf(many + used, sizeof(many) - used,
		    "%s{\"name\":\"c%zu\",\"adapter\":\"debugpy\","
		    "\"request\":\"launch\"}",
		    i ? "," : "", i);
	}
	snprintf(many + used, sizeof(many) - used, "]}");
	refused(many, "configurations");

	/* 33 arguments, one past the argv bound. */
	used = (size_t)snprintf(args, sizeof(args), "%s",
	    "{\"version\":1,\"configurations\":[{\"name\":\"a\","
	    "\"request\":\"launch\",\"adapter\":{\"command\":\"x\",\"args\":[");
	for (i = 0; i < DAP_CONFIG_MAX_ARGV; i++) {
		used += (size_t)snprintf(
		    args + used, sizeof(args) - used, "%s\"a\"", i ? "," : "");
	}
	snprintf(args + used, sizeof(args) - used, "]}}]}");
	refused(args, "command line is longer");
}

/* Members kg has never heard of are the point of the `arguments` object:
 * an adapter's launch vocabulary is its own, and a client that kept only
 * what it recognised could not debug anything new.  They survive the parse,
 * the re-encoding and the expansion. */
static void test_unknown_arguments_pass_through_opaque(void)
{
	struct dap_config_set *set = parse(
	    "{\"version\":1,\"configurations\":[{\"name\":\"a\","
	    "\"adapter\":\"debugpy\",\"request\":\"launch\",\"arguments\":"
	    "{\"someFutureKey\":{\"deep\":[1,2,{\"x\":null}]},"
	    "\"port\":5678,\"ratio\":0.5}}]}");
	char *bytes = NULL;
	size_t len = 0;

	CHECK(set != NULL);
	if (!set) {
		return;
	}
	CHECK(dap_config_expand_arguments(
		  &set->configs[0], NULL, &bytes, &len, &last_error)
	    == 0);
	if (bytes) {
		CHECK(strstr(bytes, "\"someFutureKey\"") != NULL);
		CHECK(strstr(bytes, "\"deep\":[1,2,{\"x\":null}]") != NULL);
		/* An integer stays an integer and a float stays a float:
		 * debugpy reads both and means different things by them. */
		CHECK(strstr(bytes, "\"port\":5678") != NULL);
		CHECK(strstr(bytes, "\"ratio\":0.5") != NULL);
	}
	free(bytes);
	dap_config_free(set);
}

/* debugpy errors on an explicit null where it expects a member to be
 * absent (dap-python.el:220-236), so the copy must not turn one into the
 * other in either direction. */
static void test_omission_and_null_stay_themselves(void)
{
	struct dap_config_set *set = parse(
	    "{\"version\":1,\"configurations\":[{\"name\":\"a\","
	    "\"adapter\":\"debugpy\",\"request\":\"launch\",\"arguments\":"
	    "{\"a\":null}}]}");
	struct kg_json *doc;
	char *bytes = NULL;
	size_t len = 0;

	CHECK(set != NULL);
	if (!set) {
		return;
	}
	CHECK(dap_config_expand_arguments(
		  &set->configs[0], NULL, &bytes, &len, &last_error)
	    == 0);
	doc = kg_json_parse(bytes, len, NULL);
	CHECK(kg_json_kind_of(kg_json_get(kg_json_root(doc), "a"))
	    == KG_JSON_NULL);
	CHECK(kg_json_kind_of(kg_json_get(kg_json_root(doc), "b"))
	    == KG_JSON_NONE);
	kg_json_free(doc);
	free(bytes);
	dap_config_free(set);
}

/* -------------------------------- discovery --------------------------- */

/* Sized against the mkdtemp template rather than against PATH_MAX: these
 * paths are known short, and a PATH_MAX field built from a PATH_MAX field
 * is what makes a compiler warn about a truncation that cannot happen. */
struct tree {
	char root[64];
	char deep[96];
	char source[128];
};

/* root/.kg-dap.json, root/a/b/main.c. */
static bool make_tree(struct tree *t, const char *contents)
{
	char template[] = "/tmp/kg-dapdisc-XXXXXX";
	char config[128];

	if (!mkdtemp(template)) {
		return false;
	}
	snprintf(t->root, sizeof(t->root), "%s", template);
	snprintf(t->deep, sizeof(t->deep), "%s/a/b", t->root);
	snprintf(config, sizeof(config), "%s/a", t->root);
	if (mkdir(config, 0700) != 0 || mkdir(t->deep, 0700) != 0) {
		return false;
	}
	snprintf(t->source, sizeof(t->source), "%s/main.c", t->deep);
	snprintf(
	    config, sizeof(config), "%s/%s", t->root, DAP_CONFIG_FILE_NAME);
	return write_file(t->source, "int main(void){return 0;}\n")
	    && write_file(config, contents);
}

/* The tree a discovery case built, taken away again: three directories and
 * two files, removed innermost first.  A suite that runs on every `make
 * check` does not get to leave anything in /tmp. */
static void remove_tree(const struct tree *t)
{
	char path[160];

	unlink(t->source);
	snprintf(path, sizeof(path), "%s/%s", t->root, DAP_CONFIG_FILE_NAME);
	unlink(path);
	rmdir(t->deep);
	snprintf(path, sizeof(path), "%s/a", t->root);
	rmdir(path);
	rmdir(t->root);
}

static void test_discovery_walks_up_to_the_nearest_file(void)
{
	struct tree t;
	char root[PATH_MAX];
	char path[PATH_MAX];

	CHECK(make_tree(&t, "{\"version\":1,\"configurations\":[]}"));
	CHECK(dap_config_discover(
		  t.source, root, sizeof(root), path, sizeof(path))
	    == 1);
	CHECK(strstr(path, DAP_CONFIG_FILE_NAME) != NULL);
	CHECK(strstr(root, t.root + 5) != NULL);
	CHECK(strstr(path, root) == path);
	remove_tree(&t);
}

/* Two ways to the same project must produce the same root, or a breakpoint
 * set through one path and a `${workspaceRoot}` from the other disagree. */
static void test_discovery_resolves_symlinks_consistently(void)
{
	struct tree t;
	char link[] = "/tmp/kg-daplink-XXXXXX";
	char through_link[128];
	char direct_root[PATH_MAX];
	char link_root[PATH_MAX];
	char path[PATH_MAX];

	CHECK(make_tree(&t, "{\"version\":1,\"configurations\":[]}"));
	CHECK(mkdtemp(link) != NULL);
	CHECK(rmdir(link) == 0);
	CHECK(symlink(t.root, link) == 0);
	snprintf(through_link, sizeof(through_link), "%s/a/b/main.c", link);
	CHECK(dap_config_discover(t.source, direct_root, sizeof(direct_root),
		  path, sizeof(path))
	    == 1);
	CHECK(dap_config_discover(through_link, link_root, sizeof(link_root),
		  path, sizeof(path))
	    == 1);
	CHECK(strcmp(direct_root, link_root) == 0);
	unlink(link);
	remove_tree(&t);
}

/* The two edges: a buffer that visits no file, and a path whose directory
 * does not exist.  Both are defined rather than refused -- a user who has
 * not saved yet is allowed to press the debugger's key. */
static void test_discovery_has_answers_for_unsaved_and_missing_paths(void)
{
	char root[PATH_MAX];
	char path[PATH_MAX];
	char cwd[PATH_MAX];

	CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
	CHECK(dap_config_discover(NULL, root, sizeof(root), path, sizeof(path))
	    >= 0);
	CHECK(strcmp(root, cwd) == 0 || strstr(root, cwd) != NULL);
	CHECK(dap_config_discover(
		  "*scratch*", root, sizeof(root), path, sizeof(path))
	    >= 0);
	CHECK(strcmp(root, cwd) == 0 || strstr(root, cwd) != NULL);
	CHECK(dap_config_discover("/nonexistent-kg-dir/deeper/x.c", root,
		  sizeof(root), path, sizeof(path))
	    == 0);
	CHECK(strcmp(root, "/nonexistent-kg-dir/deeper") == 0);
	CHECK(path[0] == '\0');
}

/* A configuration in the file may replace a built-in by using its name;
 * kg's own row is dropped rather than shown twice. */
static void test_load_appends_builtins_without_duplicating_names(void)
{
	struct tree t;
	struct dap_config_set *set;
	size_t i;
	size_t own = 0;

	CHECK(make_tree(&t,
	    "{\"version\":1,\"configurations\":[{\"name\":\"Python (debugpy)\","
	    "\"adapter\":\"debugpy\",\"request\":\"launch\"}]}"));
	set = dap_config_load(t.source, &last_error);
	CHECK(set != NULL);
	if (!set) {
		return;
	}
	CHECK(set->count == 3);
	CHECK(strstr(set->path, DAP_CONFIG_FILE_NAME) != NULL);
	for (i = 0; i < set->count; i++) {
		if (strcmp(set->configs[i].name, "Python (debugpy)") == 0) {
			own++;
			CHECK(!set->configs[i].builtin);
		}
	}
	CHECK(own == 1);
	dap_config_free(set);
	remove_tree(&t);
}

/* ------------------------------ substitution -------------------------- */

static struct dap_config_context test_ctx(void)
{
	static const char file[] = "/proj/src/main.c";
	static const char dir[] = "/proj/src";
	static const char root[] = "/proj";
	struct dap_config_context ctx = { file, dir, root };

	return ctx;
}

static void test_the_closed_set_expands_longest_key_first(void)
{
	struct dap_config_context ctx = test_ctx();

	CHECK(expands_to("${file}", &ctx, "/proj/src/main.c"));
	/* `${fileDir}` must never be read as `${file}` with `Dir}` left
	 * over, which is the whole reason the table is ordered. */
	CHECK(expands_to("${fileDir}", &ctx, "/proj/src"));
	CHECK(expands_to("${workspaceRoot}", &ctx, "/proj"));
	/* Adjacent substitutions, and one with no separator between. */
	CHECK(expands_to("${workspaceRoot}${fileDir}", &ctx, "/proj/proj/src"));
	CHECK(expands_to("a${file}b", &ctx, "a/proj/src/main.cb"));
	/* A `$` that begins nothing is an ordinary byte. */
	CHECK(expands_to("$HOME and $ {file}", &ctx, "$HOME and $ {file}"));
	CHECK(expands_to("", &ctx, ""));
}

static void test_unknown_and_unset_substitutions_are_errors(void)
{
	struct dap_config_context ctx = test_ctx();
	struct dap_config_context empty = { NULL, NULL, "/proj" };

	/* The typo that motivated the rule: expanding to "" would launch
	 * `/prog` and say nothing. */
	CHECK(expand("${workspaecRoot}/prog", &ctx) == NULL);
	CHECK(strstr(last_error.message, "workspaecRoot") != NULL);
	CHECK(expand("${file", &ctx) == NULL);
	CHECK(strstr(last_error.message, "never closed") != NULL);
	CHECK(unsetenv("KG_DAP_TEST_UNSET") == 0);
	CHECK(expand("${env:KG_DAP_TEST_UNSET}", &ctx) == NULL);
	CHECK(strstr(last_error.message, "not set") != NULL);
	/* A buffer visiting no file has no ${file}, and that is an error
	 * rather than an empty program path. */
	CHECK(expand("${file}", &empty) == NULL);
	CHECK(expand("${workspaceRoot}", &empty) != NULL);
}

static void test_env_substitution_is_one_pass(void)
{
	struct dap_config_context ctx = test_ctx();

	CHECK(setenv("KG_DAP_TEST_VALUE", "plain", 1) == 0);
	CHECK(expands_to("${env:KG_DAP_TEST_VALUE}", &ctx, "plain"));
	/* What a substitution expands TO is data: an environment variable
	 * holding a substitution is those characters, not another round. */
	CHECK(setenv("KG_DAP_TEST_VALUE", "${file}", 1) == 0);
	CHECK(expands_to("${env:KG_DAP_TEST_VALUE}", &ctx, "${file}"));
	CHECK(unsetenv("KG_DAP_TEST_VALUE") == 0);
}

static void test_expansion_is_bounded(void)
{
	struct dap_config_context ctx = test_ctx();
	char value[4096];
	char pattern[512];
	size_t i;

	memset(value, 'x', sizeof(value) - 1);
	value[sizeof(value) - 1] = '\0';
	CHECK(setenv("KG_DAP_TEST_BIG", value, 1) == 0);
	pattern[0] = '\0';
	for (i = 0; i < 8; i++) {
		strcat(pattern, "${env:KG_DAP_TEST_BIG}");
	}
	/* Eight copies of 4 KiB is past one expanded string's bound. */
	CHECK(expand(pattern, &ctx) == NULL);
	CHECK(strstr(last_error.message, "longer than") != NULL);
	CHECK(unsetenv("KG_DAP_TEST_BIG") == 0);
}

/* The property a textual splice would break: a value carrying JSON syntax
 * arrives as a VALUE.  Asserted by parsing the expansion back, since "is
 * this still the same string" is the question. */
static void test_substituted_values_are_never_syntax(void)
{
	struct dap_config_context ctx = test_ctx();
	char got[512];

	CHECK(setenv("KG_DAP_TEST_HOSTILE", "a\"b\\c\nd\te\xc3\xa9 {}[]", 1)
	    == 0);
	CHECK(expanded_member("{\"program\":\"${env:KG_DAP_TEST_HOSTILE}\"}",
	    &ctx, "program", got, sizeof(got)));
	CHECK(strcmp(got, "a\"b\\c\nd\te\xc3\xa9 {}[]") == 0);
	/* And in the middle of a longer string, with another substitution
	 * beside it. */
	CHECK(expanded_member(
	    "{\"program\":\"${fileDir}/${env:KG_DAP_TEST_HOSTILE}\"}", &ctx,
	    "program", got, sizeof(got)));
	CHECK(strcmp(got, "/proj/src/a\"b\\c\nd\te\xc3\xa9 {}[]") == 0);
	CHECK(unsetenv("KG_DAP_TEST_HOSTILE") == 0);
}

static void test_substitution_reaches_nested_values_but_not_keys(void)
{
	struct dap_config_context ctx = test_ctx();
	struct dap_launch_config cfg = { 0 };
	char *bytes = NULL;
	size_t len = 0;

	cfg.arguments = (char *)(uintptr_t)"{\"${file}\":[\"${fileDir}\","
					   "{\"x\":\"${workspaceRoot}\"}]}";
	cfg.arguments_len = strlen(cfg.arguments);
	CHECK(dap_config_expand_arguments(&cfg, &ctx, &bytes, &len, &last_error)
	    == 0);
	if (bytes) {
		/* Keys are copied, values are expanded. */
		CHECK(strstr(bytes, "\"${file}\":") != NULL);
		CHECK(strstr(bytes, "\"/proj/src\"") != NULL);
		CHECK(strstr(bytes, "\"x\":\"/proj\"") != NULL);
	}
	free(bytes);
}

/* ---------------------------- resolving adapters ---------------------- */

static void test_the_environment_override_replaces_the_command(void)
{
	struct dap_launch_config cfg = { 0 };
	struct dap_adapter_spec spec;

	snprintf(cfg.adapter, sizeof(cfg.adapter), "debugpy");
	CHECK(unsetenv("KG_DAP_ADAPTER_DEBUGPY") == 0);
	CHECK(dap_config_resolve_adapter(&cfg, NULL, &spec, &last_error) == 0);
	CHECK(spec.argv_count == 3 && spec.command[0] == '\0');

	CHECK(
	    setenv("KG_DAP_ADAPTER_DEBUGPY", "python3 /tmp/fake.py --mode x", 1)
	    == 0);
	CHECK(dap_config_resolve_adapter(&cfg, NULL, &spec, &last_error) == 0);
	CHECK(spec.argv_count == 0);
	CHECK(strcmp(spec.command, "python3 /tmp/fake.py --mode x") == 0);
	CHECK(unsetenv("KG_DAP_ADAPTER_DEBUGPY") == 0);
}

/* The adapter's own working directory, which is not the debuggee's: it is
 * expanded like any other configured path and reaches the spawn request. */
static void test_an_inline_adapter_keeps_its_cwd_and_transport(void)
{
	struct dap_config_context ctx = test_ctx();
	struct dap_config_set *set
	    = parse("{\"version\":1,\"configurations\":[{\"name\":\"a\","
		    "\"request\":\"attach\",\"adapter\":{\"name\":\"dlvish\","
		    "\"command\":\"adapter\",\"args\":[\"--headless\"],"
		    "\"cwd\":\"${workspaceRoot}/build\"}}]}");
	struct dap_adapter_spec spec;
	const char *argv[DAP_CONFIG_MAX_ARGV + 1];

	CHECK(set != NULL);
	if (!set) {
		return;
	}
	CHECK(set->configs[0].request == DAP_REQUEST_ATTACH);
	CHECK(set->configs[0].inline_adapter != NULL);
	CHECK(dap_config_resolve_adapter(
		  &set->configs[0], &ctx, &spec, &last_error)
	    == 0);
	CHECK(spec.transport == DAP_TRANSPORT_STDIO);
	CHECK(strcmp(spec.cwd, "/proj/build") == 0);
	CHECK(dap_adapter_spec_argv(&spec, argv, DAP_CONFIG_MAX_ARGV + 1) == 2);
	CHECK(strcmp(argv[0], "adapter") == 0);
	CHECK(strcmp(argv[1], "--headless") == 0);
	/* An inline adapter has no environment override: the fixed names
	 * belong to the built-ins. */
	CHECK(spec.env_override[0] == '\0');
	dap_config_free(set);
}

static void test_a_tcp_adapter_needs_a_port(void)
{
	struct dap_config_set *set
	    = parse("{\"version\":1,\"configurations\":[{\"name\":\"a\","
		    "\"request\":\"attach\",\"adapter\":{\"transport\":\"tcp\","
		    "\"port\":4711}}]}");

	CHECK(set != NULL);
	if (set) {
		CHECK(set->configs[0].inline_adapter->transport
		    == DAP_TRANSPORT_TCP_ATTACH);
		CHECK(set->configs[0].inline_adapter->port == 4711);
		CHECK(strcmp(set->configs[0].inline_adapter->host, "127.0.0.1")
		    == 0);
	}
	dap_config_free(set);
	refused("{\"version\":1,\"configurations\":[{\"name\":\"a\","
		"\"request\":\"attach\",\"adapter\":{\"transport\":\"tcp\"}}]}",
	    "port");
}

/* The build step is read here and run by the editor's compilation seam
 * (compilation_start_programmatic, src/compile.h). */
static void test_the_build_step_is_read_and_expanded(void)
{
	struct dap_config_context ctx = test_ctx();
	struct dap_config_set *set
	    = parse("{\"version\":1,\"configurations\":[{\"name\":\"a\","
		    "\"adapter\":\"debugpy\",\"request\":\"launch\",\"build\":"
		    "{\"command\":\"make\",\"cwd\":\"${workspaceRoot}\"}}]}");
	char expanded[PATH_MAX];

	CHECK(set != NULL);
	if (!set) {
		return;
	}
	CHECK(set->configs[0].has_build);
	CHECK(strcmp(set->configs[0].build_command, "make") == 0);
	CHECK(dap_config_expand_string(set->configs[0].build_cwd, &ctx,
		  expanded, sizeof(expanded), &last_error)
	    == 0);
	CHECK(strcmp(expanded, "/proj") == 0);
	dap_config_free(set);
}

int main(void)
{
	RUN(test_builtin_adapters_and_their_defaults);
	RUN(test_builtin_configs_when_no_file_exists);
	RUN(test_a_minimal_configuration_is_accepted);
	RUN(test_the_schema_refuses_what_it_says_it_does);
	RUN(test_duplicate_json_keys_are_refused_with_an_offset);
	RUN(test_the_bounds_are_real);
	RUN(test_unknown_arguments_pass_through_opaque);
	RUN(test_omission_and_null_stay_themselves);
	RUN(test_discovery_walks_up_to_the_nearest_file);
	RUN(test_discovery_resolves_symlinks_consistently);
	RUN(test_discovery_has_answers_for_unsaved_and_missing_paths);
	RUN(test_load_appends_builtins_without_duplicating_names);
	RUN(test_the_closed_set_expands_longest_key_first);
	RUN(test_unknown_and_unset_substitutions_are_errors);
	RUN(test_env_substitution_is_one_pass);
	RUN(test_expansion_is_bounded);
	RUN(test_substituted_values_are_never_syntax);
	RUN(test_substitution_reaches_nested_values_but_not_keys);
	RUN(test_the_environment_override_replaces_the_command);
	RUN(test_an_inline_adapter_keeps_its_cwd_and_transport);
	RUN(test_a_tcp_adapter_needs_a_port);
	RUN(test_the_build_step_is_read_and_expanded);
	return test_summary();
}
