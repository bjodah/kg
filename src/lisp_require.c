#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../fe/fe.h"
#include "def.h"
#include "lisp_internal.h"

/* ---- provide/require/featurep, and the bounded load-path require
 * searches -----------------------------------------------------------
 *
 * The feature table and load-path are both fixed C-side arrays, not Fe
 * lists: a Fe list would need re-validating (bounds, type) on every
 * (require ...), where an array the adapter alone writes needs it once,
 * at the point a native appends to it.  See lisp_internal.h's
 * struct lisp_state for the exact fields.
 *
 * Cycle detection is identity, not depth: (require ...) re-entered for a
 * feature already mid-require is an error, independent of how deep
 * LISP_MAX_LOAD_DEPTH nesting happens to be at the time. */

/* A symbol or string naming a feature, copied into a bounded buffer.
 * Accepts either the way add-hook's hook name does, since a bare Fe
 * symbol argument -- 'auto-fill -- is what every example in the sub-plan
 * and doc/lisp-api.md writes. */
static void feature_name(FeContext *context, FeObject *object, char *out,
    size_t outsize, const char *what)
{
	char *tmp;
	size_t len;

	if (FeGetType(object) != FeTSymbol && FeGetType(object) != FeTString) {
		char message[96];

		(void)snprintf(message, sizeof(message),
		    "%s: expected symbol or string", what);
		FeHandleError(context, message);
	}
	tmp = copy_fe_string(context, object, &len);
	if (len == 0 || len >= outsize) {
		free(tmp);
		FeHandleError(context, "invalid feature name");
	}
	(void)snprintf(out, outsize, "%s", tmp);
	free(tmp);
}

static bool find_feature(const char *name)
{
	size_t i;

	for (i = 0; i < state.feature_count; i++) {
		if (strcmp(state.features[i], name) == 0) {
			return true;
		}
	}
	return false;
}

FeObject *native_provide(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char name[LISP_FEATURE_NAME_MAX];

	FeRequireNoArguments(context, arguments);
	feature_name(context, object, name, sizeof(name), "provide");
	if (!find_feature(name)) {
		if (state.feature_count >= LISP_MAX_FEATURES) {
			FeHandleError(context, "feature table is full");
		}
		(void)snprintf(state.features[state.feature_count],
		    sizeof(state.features[0]), "%s", name);
		state.feature_count++;
	}
	return object;
}

FeObject *native_featurep(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char name[LISP_FEATURE_NAME_MAX];

	FeRequireNoArguments(context, arguments);
	feature_name(context, object, name, sizeof(name), "featurep");
	return FeMakeBool(context, find_feature(name));
}

/* Append DIR to the bounded load-path.  False when the table is full;
 * the caller decides whether that is silently tolerated (the default
 * entry, which is best-effort) or an error (an explicit native call). */
static bool load_path_append(const char *dir)
{
	if (state.load_path_count >= LISP_MAX_LOAD_PATH) {
		return false;
	}
	(void)snprintf(state.load_path[state.load_path_count],
	    sizeof(state.load_path[0]), "%s", dir);
	state.load_path_count++;
	return true;
}

static bool load_path_prepend(const char *dir)
{
	size_t i;

	if (state.load_path_count >= LISP_MAX_LOAD_PATH) {
		return false;
	}
	for (i = state.load_path_count; i > 0; i--) {
		memcpy(state.load_path[i], state.load_path[i - 1],
		    sizeof(state.load_path[0]));
	}
	(void)snprintf(
	    state.load_path[0], sizeof(state.load_path[0]), "%s", dir);
	state.load_path_count++;
	return true;
}

/* Seed the default <config>/kg/lisp/ entry, exactly once, however the
 * default entry ends up ordered relative to explicit
 * (add-to-load-path ...) calls: this runs at the top of both
 * native_require and native_add_to_load_path, so whichever runs first
 * still leaves the default entry appended (searched after) rather than
 * prepended in front of a directory the caller has not added yet.  No
 * default entry is not an error -- $XDG_CONFIG_HOME/$HOME unset just
 * means an empty load-path until something is added to it. */
static void ensure_default_load_path(void)
{
	char dir[PATH_MAX];

	if (state.load_path_ready) {
		return;
	}
	state.load_path_ready = true;
	if (lisp_config_path(dir, sizeof(dir), "lisp") == 0) {
		(void)load_path_append(dir);
	}
}

FeObject *native_add_to_load_path(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char *dir;
	size_t len;

	FeRequireNoArguments(context, arguments);
	ensure_default_load_path();
	dir = copy_fe_string(context, object, &len);
	if (len == 0 || len >= sizeof(state.load_path[0])) {
		free(dir);
		FeHandleError(context, "invalid load-path directory");
	}
	if (!load_path_prepend(dir)) {
		free(dir);
		FeHandleError(context, "load-path is full");
	}
	free(dir);
	return FeNil(context);
}

/* PATH_MAX candidate = "DIR/STEM.fe"; true and filled in when that file
 * is at least readable.  A directory that does not exist fails the same
 * access() call a missing file would, so a load-path full of stale
 * directories is just several misses in a row, not a special case. */
static bool candidate_readable(
    const char *dir, const char *stem, char *out, size_t outsize)
{
	int n = snprintf(out, outsize, "%s/%s.fe", dir, stem);

	return n >= 0 && (size_t)n < outsize && access(out, R_OK) == 0;
}

/* Resolve STEM to a file path: a name containing '/' is literal, exactly
 * as (load ...) treats one; otherwise search the load-path in order for
 * "STEM.fe", first match wins. */
static void resolve_require_path(
    FeContext *context, const char *stem, char *out, size_t outsize)
{
	size_t i;

	if (strchr(stem, '/')) {
		if ((size_t)snprintf(out, outsize, "%s", stem) >= outsize) {
			FeHandleError(context, "path is too long");
		}
		return;
	}
	ensure_default_load_path();
	for (i = 0; i < state.load_path_count; i++) {
		if (candidate_readable(
			state.load_path[i], stem, out, outsize)) {
			return;
		}
	}
	command_error(context, "cannot find in load-path", stem);
}

static int requiring_index(const char *name)
{
	size_t i;

	for (i = 0; i < state.requiring_depth; i++) {
		if (strcmp(state.requiring[i], name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* (require FEATURE &optional FILENAME): a no-op, returning FEATURE
 * unevaluated, when it is already provided.  Otherwise FILENAME (or
 * FEATURE's own name, absent one) resolves through the load-path exactly
 * as native_load resolves a bare name, the file is evaluated nested in
 * this call the way (load ...) nests, and the feature must be provided
 * by the time it returns -- a file that runs to completion without
 * calling (provide ...) is a caller bug, reported rather than silently
 * accepted. */
FeObject *native_require(FeContext *context, FeObject *arguments)
{
	FeObject *feature_object = FeGetNextArgument(context, &arguments);
	FeObject *filename_object = nullptr;
	char name[LISP_FEATURE_NAME_MAX];
	char stem[PATH_MAX];
	char path[PATH_MAX];

	if (!FeIsNil(arguments)) {
		filename_object = FeGetNextArgument(context, &arguments);
	}
	FeRequireNoArguments(context, arguments);
	feature_name(context, feature_object, name, sizeof(name), "require");
	if (find_feature(name)) {
		return feature_object;
	}
	if (filename_object != nullptr && !FeIsNil(filename_object)) {
		feature_name(
		    context, filename_object, stem, sizeof(stem), "require");
	} else {
		(void)snprintf(stem, sizeof(stem), "%s", name);
	}
	if (requiring_index(name) >= 0) {
		command_error(context, "cyclic require", name);
	}
	if (state.requiring_depth >= LISP_MAX_REQUIRE_STACK) {
		FeHandleError(context, "require nesting too deep");
	}
	(void)snprintf(state.requiring[state.requiring_depth],
	    sizeof(state.requiring[0]), "%s", name);
	state.requiring_depth++;

	resolve_require_path(context, stem, path, sizeof(path));
	lisp_eval_file(context, path);

	state.requiring_depth--;
	if (!find_feature(name)) {
		command_error(context, "did not provide feature", name);
	}
	return feature_object;
}
