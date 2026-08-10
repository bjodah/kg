#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../fe/fe.h"
#include "bufmgr.h"
#include "cmd.h"
#include "def.h"
#include "event.h"
#include "lisp_internal.h"
#include "prompt.h"

/* ---- Minibuffer reads from Lisp ---------------------------------------
 *
 * Two callers, one seam.  `(interactive "s...")` and friends reach the
 * four readers below through lisp_core.c's code table; Phase 16's public
 * `read-string', `read-number', `read-file-name', `read-buffer',
 * `y-or-n-p', `yes-or-no-p' and `completing-read' reach the same readers
 * with the same re-entrancy rule, so a value a command's argument list
 * can hold is a value its body can ask for, and neither route can
 * acquire a policy the other lacks.
 *
 * Everything a read needs is on this C frame, bounded, and copied out of
 * Fe before the prompt opens: a heap copy held across a cancellation
 * leaks, because a cancelled prompt raises `quit' straight past every
 * frame between here and the enclosing condition-case.  For the same
 * reason argument validation happens BEFORE the prompt seam is asked
 * for, which is also what makes it testable without a terminal.
 *
 * Prompts are literal, as the interactive codes' are: kg does not pass
 * prompt text through `format', and that divergence is recorded once,
 * for both routes (see the phase7-interactive-prompt-literal row). */

/* The longest prompt text a read accepts.  Bounded for the frame reason
 * above; the echo area shows far less than this anyway. */
static constexpr int lisp_prompt_max = 256;
/* And the longest answer, matching the interactive codes' own buffers. */
static constexpr int lisp_answer_max = PATH_MAX;

void lisp_prompt_require(FeContext *context)
{
	if (cmd_prompt_fd() < 0 || kg_event_prompt_active()) {
		FeHandleError(
		    context, "interactive prompt is not available here");
	}
}

/* The descriptor every read below uses, or a raise when there is no live
 * command prompt to read from. */
static int lisp_prompt_fd(FeContext *context)
{
	lisp_prompt_require(context);
	return cmd_prompt_fd();
}

typedef void (*prompt_result_handler)(FeContext *context, const char *kind);

static void prompt_result_ok(FeContext *context, const char *kind)
{
	(void)context;
	(void)kind;
}

/* C-g is Emacs' `quit', catchable by a condition-case naming `quit' and
 * by nothing else -- the one behaviour every prompting form shares. */
[[noreturn]] static void prompt_result_quit(
    FeContext *context, const char *kind)
{
	(void)kind;
	FeRaiseCompletion(context, FeCompletionQuit, "Quit");
}

[[noreturn]] static void prompt_result_overflow(
    FeContext *context, const char *kind)
{
	char message[96];

	(void)snprintf(message, sizeof(message),
	    "interactive prompt overflow for %s", kind);
	FeHandleError(context, message);
}

static void prompt_result_check(
    FeContext *context, enum minibuf_result result, const char *kind)
{
	static const prompt_result_handler handlers[] = {
		prompt_result_quit,
		prompt_result_ok,
		prompt_result_overflow,
	};

	handlers[result + 1](context, kind);
}

/* ---- Argument helpers -------------------------------------------------
 *
 * Fe raises "too few arguments" when a native reads past the end of its
 * list, so every Emacs &optional argument is guarded rather than read. */
static FeObject *optional_argument(FeContext *context, FeObject **arguments)
{
	return FeIsNil(*arguments) ? FeNil(context)
				   : FeGetNextArgument(context, arguments);
}

/* Copy a required string argument onto the caller's frame. */
static void copy_string_argument(FeContext *context, FeObject *object,
    char *out, size_t size, const char *what)
{
	size_t length;
	char message[96];

	lisp_check_string(context, object);
	length = FeStringByteLength(context, object);
	if (length >= size) {
		(void)snprintf(
		    message, sizeof(message), "%s is too long", what);
		FeHandleError(context, message);
	}
	if (length > 0 && !FeCopyStringBytes(context, object, out, length)) {
		FeHandleError(context, "cannot copy string");
	}
	out[length] = '\0';
}

/* The same for an &optional string: nil leaves `out` empty. */
static void copy_optional_string(FeContext *context, FeObject *object,
    char *out, size_t size, const char *what)
{
	out[0] = '\0';
	if (!FeIsNil(object)) {
		copy_string_argument(context, object, out, size, what);
	}
}

/* Append to a bounded frame buffer, raising rather than truncating: a
 * path built out of a DIR and an INITIAL that did not fit is not the path
 * the caller asked for. */
static void append_text(
    FeContext *context, char *out, size_t size, const char *text)
{
	size_t have = strlen(out);
	size_t add = strlen(text);

	if (have + add >= size) {
		FeHandleError(context, "file name is too long");
	}
	memcpy(out + have, text, add + 1);
}

/* An &optional argument kg accepts only as nil, because the behaviour it
 * selects is not one kg has.  Loud rather than silently ignored: a
 * PREDICATE that is quietly dropped answers with candidates the caller
 * excluded, which is worse than saying so. */
static void reject_unsupported(
    FeContext *context, FeObject *object, const char *what)
{
	char message[96];

	if (!FeIsNil(object)) {
		(void)snprintf(message, sizeof(message),
		    "%s is not supported by kg", what);
		FeHandleError(context, message);
	}
}

/* An answer of "" means "the caller's default", which is what every
 * DEFAULT-shaped argument here delivers; with no default it stays the
 * empty string, exactly as Emacs' own read-string does. */
static FeObject *answer_or_default(
    FeContext *context, const char *text, const struct lisp_read_options *opt)
{
	if (text[0] == '\0' && opt->fallback != nullptr
	    && !FeIsNil(opt->fallback)) {
		return opt->fallback;
	}
	return FeMakeString(context, text);
}

/* ---- The four readers ------------------------------------------------- */

FeObject *lisp_read_string_prompt(FeContext *context, int fd,
    const char *prompt, const struct lisp_read_options *opt)
{
	char text[lisp_answer_max];
	enum minibuf_result result;

	(void)snprintf(
	    text, sizeof(text), "%s", opt->initial ? opt->initial : "");
	result = editor_read_line(fd, prompt, text, sizeof(text));
	prompt_result_check(context, result, opt->kind);
	return answer_or_default(context, text, opt);
}

FeObject *lisp_read_number_prompt(FeContext *context, int fd,
    const char *prompt, const struct lisp_read_options *opt)
{
	char text[256];

	for (;;) {
		enum minibuf_result result;
		enum kg_number_token kind;

		text[0] = '\0';
		result = editor_read_line(fd, prompt, text, sizeof(text));
		prompt_result_check(context, result, opt->kind);
		if (text[0] == '\0' && opt->fallback != nullptr
		    && !FeIsNil(opt->fallback)) {
			return opt->fallback;
		}
		kind = kg_number_token_classify(text);
		if (kind == KG_NUMBER_TOKEN_INTEGER) {
			intmax_t integer;

			errno = 0;
			integer = strtoimax(text, nullptr, 10);
			/* Past int64 it becomes a double, as an integer
			 * literal does in fe's own reader; kg has no
			 * bignums, which is already a recorded divergence. */
			if (errno != ERANGE) {
				return FeMakeInteger(context, (int64_t)integer);
			}
			return FeMakeDouble(context, strtod(text, nullptr));
		}
		if (kind == KG_NUMBER_TOKEN_FLOAT) {
			return FeMakeDouble(context, strtod(text, nullptr));
		}
		editor_set_status_message("Please enter a number.");
	}
}

FeObject *lisp_read_path_prompt(FeContext *context, int fd, const char *prompt,
    const struct lisp_read_options *opt)
{
	char text[lisp_answer_max];
	struct stat st;
	enum minibuf_result result;

	if (opt->initial != nullptr && opt->initial[0] != '\0') {
		(void)snprintf(text, sizeof(text), "%s", opt->initial);
	} else {
		editor_prompt_prefill_dir(text, sizeof(text));
	}
	for (;;) {
		result = editor_read_line_path(fd, prompt, text, sizeof(text));
		prompt_result_check(context, result, opt->kind);
		if (!opt->must_match || stat(text, &st) == 0) {
			return answer_or_default(context, text, opt);
		}
		editor_set_status_message("File does not exist: %s", text);
	}
}

FeObject *lisp_read_buffer_prompt(FeContext *context, int fd,
    const char *prompt, const struct lisp_read_options *opt)
{
	char text[lisp_answer_max];
	enum minibuf_result result
	    = buf_read_name(fd, prompt, text, sizeof(text),
		opt->must_match ? BUF_NAME_EXISTING : BUF_NAME_ANY, nullptr);

	prompt_result_check(context, result, opt->kind);
	return FeMakeString(context, text);
}

/* ---- The public forms -------------------------------------------------
 *
 * Emacs' argument lists, honoured as far as kg has substance for them.
 * A HISTORY argument is accepted and ignored: kg's minibuffer histories
 * are per-call-site C rings (shell commands, compile commands), not
 * values a Lisp symbol names, so there is nothing for the argument to
 * select -- recorded as a divergence rather than raised, since raising
 * would make the Emacs spelling of an ordinary call fail. */

/* (read-string PROMPT &optional INITIAL-INPUT HISTORY DEFAULT-VALUE) */
FeObject *native_read_string(FeContext *context, FeObject *arguments)
{
	FeObject *prompt = FeGetNextArgument(context, &arguments);
	FeObject *initial = optional_argument(context, &arguments);
	FeObject *history = optional_argument(context, &arguments);
	FeObject *fallback = optional_argument(context, &arguments);
	char prompt_text[lisp_prompt_max];
	char initial_text[lisp_answer_max];
	struct lisp_read_options opt
	    = { initial_text, fallback, false, "read-string" };

	FeRequireNoArguments(context, arguments);
	(void)history;
	copy_string_argument(
	    context, prompt, prompt_text, sizeof(prompt_text), "prompt");
	copy_optional_string(context, initial, initial_text,
	    sizeof(initial_text), "initial input");
	if (!FeIsNil(fallback)) {
		lisp_check_string(context, fallback);
	}
	return lisp_read_string_prompt(
	    context, lisp_prompt_fd(context), prompt_text, &opt);
}

/* (read-number PROMPT &optional DEFAULT HISTORY).  The default is shown
 * the way Emacs' own read-number shows it, and an empty answer takes
 * it; with no default an empty answer re-prompts, which is the `n'
 * interactive code's behaviour. */
FeObject *native_read_number(FeContext *context, FeObject *arguments)
{
	FeObject *prompt = FeGetNextArgument(context, &arguments);
	FeObject *fallback = optional_argument(context, &arguments);
	FeObject *history = optional_argument(context, &arguments);
	char prompt_text[lisp_prompt_max];
	char shown[lisp_prompt_max + 96];
	char printed[64];
	struct lisp_read_options opt
	    = { nullptr, fallback, false, "read-number" };

	FeRequireNoArguments(context, arguments);
	(void)history;
	copy_string_argument(
	    context, prompt, prompt_text, sizeof(prompt_text), "prompt");
	if (FeIsNil(fallback)) {
		return lisp_read_number_prompt(
		    context, lisp_prompt_fd(context), prompt_text, &opt);
	}
	if (FeGetType(fallback) != FeTDouble
	    && FeGetType(fallback) != FeTInteger) {
		lisp_raise_wrong_type(context, "numberp", fallback);
	}
	(void)FeToString(context, fallback, printed, sizeof(printed));
	(void)snprintf(
	    shown, sizeof(shown), "%s(default %s) ", prompt_text, printed);
	return lisp_read_number_prompt(
	    context, lisp_prompt_fd(context), shown, &opt);
}

/* (read-file-name PROMPT &optional DIR DEFAULT-FILENAME MUSTMATCH INITIAL
 *  PREDICATE).  DIR and INITIAL together are the prompt's initial text --
 * kg's path prompt is prefilled rather than carrying a separate
 * directory -- and with neither the prompt starts where the current
 * buffer's file lives, as C-x C-f does. */
FeObject *native_read_file_name(FeContext *context, FeObject *arguments)
{
	FeObject *prompt = FeGetNextArgument(context, &arguments);
	FeObject *dir = optional_argument(context, &arguments);
	FeObject *fallback = optional_argument(context, &arguments);
	FeObject *must_match = optional_argument(context, &arguments);
	FeObject *initial = optional_argument(context, &arguments);
	FeObject *predicate = optional_argument(context, &arguments);
	char prompt_text[lisp_prompt_max];
	char initial_text[lisp_answer_max];
	char prefill[lisp_answer_max];
	struct lisp_read_options opt
	    = { prefill, fallback, !FeIsNil(must_match), "read-file-name" };

	FeRequireNoArguments(context, arguments);
	copy_string_argument(
	    context, prompt, prompt_text, sizeof(prompt_text), "prompt");
	copy_optional_string(
	    context, dir, prefill, sizeof(prefill), "directory");
	copy_optional_string(context, initial, initial_text,
	    sizeof(initial_text), "initial input");
	if (!FeIsNil(fallback)) {
		lisp_check_string(context, fallback);
	}
	reject_unsupported(context, predicate, "read-file-name PREDICATE");
	if (prefill[0] != '\0' && prefill[strlen(prefill) - 1] != '/') {
		append_text(context, prefill, sizeof(prefill), "/");
	}
	append_text(context, prefill, sizeof(prefill), initial_text);
	return lisp_read_path_prompt(
	    context, lisp_prompt_fd(context), prompt_text, &opt);
}

/* (read-buffer PROMPT &optional DEFAULT REQUIRE-MATCH PREDICATE).  DEFAULT
 * is accepted and ignored: kg's buffer picker answers an empty query with
 * its own default -- the current buffer when a match is required, the
 * next buffer in the ring otherwise -- and never returns the empty text a
 * caller's default would have to replace. */
FeObject *native_read_buffer(FeContext *context, FeObject *arguments)
{
	FeObject *prompt = FeGetNextArgument(context, &arguments);
	FeObject *fallback = optional_argument(context, &arguments);
	FeObject *require_match = optional_argument(context, &arguments);
	FeObject *predicate = optional_argument(context, &arguments);
	char prompt_text[lisp_prompt_max];
	struct lisp_read_options opt
	    = { nullptr, nullptr, !FeIsNil(require_match), "read-buffer" };

	FeRequireNoArguments(context, arguments);
	copy_string_argument(
	    context, prompt, prompt_text, sizeof(prompt_text), "prompt");
	if (!FeIsNil(fallback)) {
		lisp_check_symbol_or_string(context, fallback);
	}
	reject_unsupported(context, predicate, "read-buffer PREDICATE");
	return lisp_read_buffer_prompt(
	    context, lisp_prompt_fd(context), prompt_text, &opt);
}

/* (y-or-n-p PROMPT): one key, kg's own confirmation policy -- `y' is yes
 * and every other key is no, where Emacs re-asks -- except that C-g is
 * `quit' rather than an answer. */
FeObject *native_y_or_n_p(FeContext *context, FeObject *arguments)
{
	FeObject *prompt = FeGetNextArgument(context, &arguments);
	char prompt_text[lisp_prompt_max];
	char question[lisp_prompt_max + 16];
	enum prompt_yn answer;

	FeRequireNoArguments(context, arguments);
	copy_string_argument(
	    context, prompt, prompt_text, sizeof(prompt_text), "prompt");
	(void)snprintf(question, sizeof(question), "%s(y or n) ", prompt_text);
	answer = prompt_ask_yn(lisp_prompt_fd(context), question);
	if (answer == PROMPT_YN_CANCELLED) {
		prompt_result_check(context, MINIBUF_CANCELLED, "y-or-n-p");
	}
	return FeMakeBool(context, answer == PROMPT_YN_YES);
}

/* ASCII case-insensitive equality against one of the two literal words.
 * Not ctype's: kg never calls setlocale(), and the grammar here is ASCII
 * by definition (def.h's rule). */
static bool word_is(const char *text, const char *word)
{
	size_t i;

	for (i = 0; word[i] != '\0'; i++) {
		char c = text[i];

		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		if (c != word[i]) {
			return false;
		}
	}
	return text[i] == '\0';
}

/* (yes-or-no-p PROMPT): the typed word, re-prompting until one of the two
 * words is answered.  Case-insensitive, as Emacs' is. */
FeObject *native_yes_or_no_p(FeContext *context, FeObject *arguments)
{
	FeObject *prompt = FeGetNextArgument(context, &arguments);
	char prompt_text[lisp_prompt_max];
	char question[lisp_prompt_max + 16];
	char text[64];
	int fd;

	FeRequireNoArguments(context, arguments);
	copy_string_argument(
	    context, prompt, prompt_text, sizeof(prompt_text), "prompt");
	(void)snprintf(
	    question, sizeof(question), "%s(yes or no) ", prompt_text);
	fd = lisp_prompt_fd(context);
	for (;;) {
		enum minibuf_result result;

		text[0] = '\0';
		result = editor_read_line(fd, question, text, sizeof(text));
		prompt_result_check(context, result, "yes-or-no-p");
		if (word_is(text, "yes")) {
			return FeMakeBool(context, true);
		}
		if (word_is(text, "no")) {
			return FeMakeBool(context, false);
		}
		editor_set_status_message("Please answer yes or no.");
	}
}

/* The COLLECTION, copied onto this frame: bounded, because the picker can
 * only show and cycle PROMPT_CHOICE_MAX of them, and copied, because
 * every byte the picker reads has to outlive the Fe list without a heap
 * allocation a cancellation would leak. */
struct completion_table {
	char text[PROMPT_CHOICE_MAX][128];
	const char *names[PROMPT_CHOICE_MAX];
	int n;
};

static void collect_choices(
    FeContext *context, FeObject *collection, struct completion_table *table)
{
	FeObject *rest = collection;

	table->n = 0;
	while (!FeIsNil(rest)) {
		if (FeGetType(rest) != FeTPair) {
			lisp_raise_wrong_type(context, "listp", collection);
		}
		if (table->n >= PROMPT_CHOICE_MAX) {
			FeHandleError(
			    context, "completing-read COLLECTION is too long");
		}
		copy_string_argument(context, FeCar(context, rest),
		    table->text[table->n], sizeof(table->text[0]),
		    "completion candidate");
		table->names[table->n] = table->text[table->n];
		table->n++;
		rest = FeCdr(context, rest);
	}
}

/* (completing-read PROMPT COLLECTION &optional PREDICATE REQUIRE-MATCH
 *  INITIAL-INPUT HISTORY DEFAULT).  COLLECTION is a list of strings;
 * Emacs' alists, obarrays, hash tables and functions are not kg's, and
 * PREDICATE is refused rather than dropped. */
FeObject *native_completing_read(FeContext *context, FeObject *arguments)
{
	FeObject *prompt = FeGetNextArgument(context, &arguments);
	FeObject *collection = FeGetNextArgument(context, &arguments);
	FeObject *predicate = optional_argument(context, &arguments);
	FeObject *require_match = optional_argument(context, &arguments);
	FeObject *initial = optional_argument(context, &arguments);
	FeObject *history = optional_argument(context, &arguments);
	FeObject *fallback = optional_argument(context, &arguments);
	char prompt_text[lisp_prompt_max];
	char initial_text[PROMPT_CHOICE_QUERY_MAX];
	char answer[lisp_answer_max];
	struct completion_table table;
	struct lisp_read_options opt
	    = { nullptr, fallback, false, "completing-read" };
	enum minibuf_result result;

	FeRequireNoArguments(context, arguments);
	(void)history;
	copy_string_argument(
	    context, prompt, prompt_text, sizeof(prompt_text), "prompt");
	reject_unsupported(context, predicate, "completing-read PREDICATE");
	copy_optional_string(context, initial, initial_text,
	    sizeof(initial_text), "initial input");
	if (!FeIsNil(fallback)) {
		lisp_check_string(context, fallback);
	}
	collect_choices(context, collection, &table);
	result = prompt_read_choice(lisp_prompt_fd(context), prompt_text,
	    table.names, table.n, initial_text, !FeIsNil(require_match), answer,
	    sizeof(answer));
	prompt_result_check(context, result, "completing-read");
	return answer_or_default(context, answer, &opt);
}
