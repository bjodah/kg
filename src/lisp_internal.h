#ifndef KG_LISP_INTERNAL_H
#define KG_LISP_INTERNAL_H

/* The private shared surface of the Lisp adapter.  Only src/lisp_*.c may
 * include this header; everything here is implementation, and nothing of
 * it leaks into the public src/lisp.h.  It is one of the src headers
 * `make header-check` compiles on its own, so it includes fe.h and the
 * standard headers it needs and nothing more. */

#include <setjmp.h>
#include <stddef.h>

#include "../fe/fe.h"
#include "def.h"
#include "lisp.h"
#include "lisp_locals.h"
#include "lisp_obj.h"
#include "marker.h"
#include "perf.h"
#include "regex.h"

/* Adapter limits, shared by struct lisp_state and the modules that walk
 * its tables.  Macros rather than constexpr because struct lisp_state's
 * array bounds, and every translation unit that includes the header,
 * need the value. */
#define LISP_MAX_LOAD_DEPTH 8
#define LISP_MAX_COMMANDS 32
#define LISP_COMMAND_NAME_MAX 64
/* provide/require/featurep (lisp_require.c).  Cycle detection is a
 * separate bounded stack from LISP_MAX_LOAD_DEPTH: that macro caps how
 * deeply (load ...)/(require ...) nest, this one caps how many features
 * can be mid-require at once, which is the same number in practice but a
 * different question -- identity, not depth. */
#define LISP_MAX_FEATURES 32
#define LISP_FEATURE_NAME_MAX 64
#define LISP_MAX_LOAD_PATH 8
#define LISP_MAX_REQUIRE_STACK 8
#define LISP_INTERACTIVE_MAX_ARGS 16
/* The longest `(interactive "…")` specification string.  Bounded, and on
 * the argument builder's own C frame, because everything it drives can
 * raise: a heap copy held across a prompt cancellation leaks. */
#define LISP_INTERACTIVE_SPEC_MAX 512

enum lisp_interactive_kind {
	LISP_INTERACTIVE_NONE,
	LISP_INTERACTIVE_STRING,
	LISP_INTERACTIVE_FORM,
};

/* A Lisp-defined command: the function object stays alive through its
 * root for the registration lifetime and is released on redefinition or
 * removal. */
struct lisp_command {
	char name[LISP_COMMAND_NAME_MAX]; /* empty marks a free slot */
	struct FeRoot *function_root;
	struct FeRoot *interactive_root;
	struct FeRoot *documentation_root;
	/* The RAW `(interactive ...)` specification, beside the thunk-or-
	 * string `interactive_root` holds.  07D stored only the latter, and
	 * for a FORM spec that is a closure over the descriptor -- callable,
	 * but with nothing to show a reader, which is why `interactive-form`
	 * could not exist and `commandp` had to answer about a NAME.  Nil
	 * for a command defined by a direct `define-command` that did not
	 * pass one; `interactive-form` then rebuilds what it can from the
	 * kind. */
	struct FeRoot *interactive_form_root;
	enum lisp_interactive_kind interactive_kind;
};

struct lisp_frame {
	jmp_buf error_jump;
	size_t gc_checkpoint;
};

struct lisp_prefix_binding {
	FeContext *context;
	FeObject *symbol;
	struct FeRoot *old_root;
	struct FeRoot *new_root;
	int active;
	/* The binding this one displaced, innermost first.  A nested
	 * (command-execute ...) makes a second binding while the outer
	 * command's is live, and state.prefix_binding is one slot: without
	 * the chain the outer binding was simply forgotten, and frame
	 * recovery -- which drains from that slot -- freed nothing. */
	struct lisp_prefix_binding *outer;
};

/* The runtime execution context for one frame: which buffer Lisp code is
 * working in.  Separate from the active-window globals; hidden-buffer
 * operations move it, never a window.  Initialized from the active window
 * at frame entry (lisp_exec_enter) and synced back on success
 * (lisp_exec_leave).
 *
 * Point is deliberately not part of the frame.  Emacs point is a property
 * of the *buffer*, not of whichever frame happens to be looking at it: a
 * single frame-scoped point loses (goto-char N) across an intervening
 * (set-buffer other), and forgets where a previous frame left off in a
 * hidden buffer.  So the runtime point lives in struct lisp_point_table
 * below, keyed by buffer handle, and outlives the frame. */
struct kg_lisp_exec_ctx {
	struct kg_buffer_handle buffer; /* selected buffer for this frame */
};

/* One buffer's runtime point.  `active` false marks a free table slot;
 * `buffer` and `point` are meaningless until then. */
struct kg_lisp_point_entry {
	bool active;
	struct kg_buffer_handle buffer;
	struct kg_marker_handle point; /* right gravity, per Plan 03 */
};

/* Bounded by MAX_BUFFERS: a frame can never have more distinct buffers to
 * remember point for than exist, so a slot is always available once
 * entries whose buffer has since been killed are reclaimed -- which is
 * exactly what makes room after a kill. */
struct lisp_point_table {
	struct kg_lisp_point_entry entries[MAX_BUFFERS];
};

/* The last successful search's capture data.  Outlives the frame so
 * (re-search-forward ...) in one eval and (match-beginning 0) in the
 * next work correctly -- the same treatment as per-buffer point.
 * Not a global: only src/lisp_search.c reads or writes it, and
 * lisp_internal.h is the private surface.  See Phase 3 notes. */
struct kg_lisp_match_data {
	bool valid;
	/* What the last match ran against, and therefore what `match'
	 * holds.  A BUFFER match (search-forward and friends) stores byte
	 * columns of `row' in `buffer', converted to positions when
	 * match-beginning/-end read them.  A STRING match (string-match)
	 * stores Emacs' own units -- 0-based CHARACTER indices into the
	 * subject -- because string-match is the last place the subject is
	 * reachable, and `buffer'/`row' are meaningless for it. */
	bool on_string;
	struct kg_buffer_handle buffer;
	int row;
	struct kg_match match;
};

struct lisp_state {
	void *arena;
	FeContext *context;
	struct lisp_frame frame;
	char error[1024];
	/* The in-flight completion kind: written by the Fe error callback,
	 * read by the seam handling it, and disarmed by
	 * lisp_settle_completion() once that seam is done. */
	FeCompletion error_kind;
	/* ... whose fe-free mirror this is, latched for
	 * kg_lisp_last_error_kind() so a caller outside the adapter tells a
	 * quit from an error by kind and not by reading the message. */
	enum kg_lisp_error_kind last_error_kind;
	int (*interrupt_check)(void);
	/* Source buffers owned by in-flight (load ...) calls.  Fe errors
	 * longjmp past the natives, so frame recovery frees the leftovers. */
	char *load_buffers[LISP_MAX_LOAD_DEPTH];
	size_t load_depth;
	/* The reader cursor and input-unit bookkeeping beside each
	 * load_buffers slot, one struct per nesting level (Phase 12's fix
	 * cycle): `load' and `require' are prelude loops over
	 * internal--read-form, so the C side keeps, per open stream, the
	 * byte/line cursor FeReadInputForm advances, the path that is the
	 * unit's label (borrowed by fe for the unit's whole lifetime, which
	 * is why it is per-slot storage and not a stack local), and the
	 * enclosing FeInputUnit to restore on close. */
	struct lisp_load_stream {
		size_t offset;
		size_t line;
		size_t size;
		bool open;
		char path[PATH_MAX];
		FeInputUnit enclosing;
	} load_streams[LISP_MAX_LOAD_DEPTH];
	/* Buffer text extracted by a native that has not handed it to Fe
	 * yet; freed by frame recovery for the same reason. */
	char *scratch;
	struct lisp_command commands[LISP_MAX_COMMANDS];
	/* The execution context for the current frame, see above. */
	struct kg_lisp_exec_ctx exec;
	/* Every buffer's runtime point, independent of any frame. */
	struct lisp_point_table points;
	/* The last search's captures (outlives the frame). */
	struct kg_lisp_match_data match;
	/* The bounded pool of Fe-visible editor objects. */
	struct lisp_object_pool object_pool;
	/* Every buffer-local variable binding, and which buffer's are in
	 * force right now (src/lisp_locals.h). */
	struct lisp_locals_table locals;
	/* provide/require/featurep's feature table: names registered by
	 * (provide ...), checked by (featurep ...) and (require ...). */
	char features[LISP_MAX_FEATURES][LISP_FEATURE_NAME_MAX];
	size_t feature_count;
	/* Bounded load-path: C-side directories (require)'s search resolves,
	 * in order.  Not a Fe list -- see lisp_require.c.  load_path_ready
	 * marks whether the default <config>/kg/lisp/ entry has already been
	 * seeded, which happens at most once regardless of call order between
	 * (require ...) and (add-to-load-path ...). */
	char load_path[LISP_MAX_LOAD_PATH][PATH_MAX];
	size_t load_path_count;
	bool load_path_ready;
	/* Features currently mid-(require ...), innermost last: cycle
	 * detection.  Each require registers an Fe cleanup so a Lisp-level
	 * condition-case pop happens too; outer frame recovery resets the count
	 * as a final fallback when a completion reaches kg's host boundary. */
	char requiring[LISP_MAX_REQUIRE_STACK][LISP_FEATURE_NAME_MAX];
#if KG_PERF_COUNTERS
	/* When the OUTERMOST require of a chain started, for the
	 * package-load counter: latched by internal--require-push at depth
	 * 1, banked by internal--require-check at the same depth.  Counting
	 * builds only, like every KG_PERF field. */
	long long require_outer_before_ns;
#endif
	size_t requiring_depth;
	bool frame_active;
	struct lisp_prefix_binding *prefix_binding;
	/* The value the command activation in flight produced, handed to
	 * (command-execute ...) by lisp_take_command_value().  See that
	 * function for why this is a bare pointer and not a root. */
	FeObject *command_value;
	bool initialized;
};

extern struct lisp_state state;
/* The step-budget/interrupt/GC options every evaluation and the prelude
 * share. */
extern const FeEvalOptions eval_options;

/* Error translation and scratch helpers (lisp_core.c). */
void set_error(const char *format, ...);
void copy_result(char *result, size_t result_size, const char *text);
[[noreturn]] void command_error(
    FeContext *context, const char *prefix, const char *name);
void release_scratch(void);
char *copy_fe_string(FeContext *context, FeObject *object, size_t *length);
/* Resolve a function designator to the object it names (lisp_core.c): a
 * symbol reads its function cell, anything else passes through unchanged.
 * Shared by hooks, process filters/sentinels and functionp, which used to
 * spell the lookup out at three call sites. */
FeObject *lisp_function_designator(FeContext *context, FeObject *object);
/* The host-call-site form of the same rule (lisp_core.c): resolves as
 * above, then answers whether FeCall can call the result.  Returns nullptr
 * with `diagnostic` set to `void-function NAME` / `invalid-function NAME`
 * instead of raising, because a raise from this point is not contained by
 * the caller's guarded frame -- see the definition's comment. */
FeObject *lisp_callable_designator(FeContext *context, FeObject *object,
    char *diagnostic, size_t diagnostic_size);
#define LISP_CALLABLE_DIAGNOSTIC_MAX 128
/* Raise Emacs' (wrong-type-argument PREDICATE VALUE) as a real condition a
 * handler naming wrong-type-argument can catch (lisp_core.c). */
[[noreturn]] void lisp_raise_wrong_type(
    FeContext *context, const char *predicate, FeObject *value);
/* Raise Emacs' file conditions -- (SYMBOL OPERATION STRERROR PATH), with
 * SYMBOL one of file-missing/file-error -- as a real condition a handler
 * naming either class, or `error', can catch (lisp_core.c). */
[[noreturn]] void lisp_raise_file_condition(FeContext *context,
    const char *symbol, const char *operation, const char *detail,
    const char *path);
/* Raise Emacs' (args-out-of-range FIRST SECOND) -- the range failure that
 * is not a type failure, whose data is the offending values themselves
 * with no predicate in front (lisp_core.c). */
[[noreturn]] void lisp_raise_args_out_of_range(
    FeContext *context, FeObject *first, FeObject *second);
/* Raise Emacs' `(void-variable SYMBOL)' -- what a reference to a name with
 * no value answers, and therefore what `default-value' and
 * `buffer-local-value' must answer rather than inventing a nil
 * (lisp_core.c). */
[[noreturn]] void lisp_raise_void_variable(
    FeContext *context, FeObject *symbol);
/* Raise Emacs' `(setting-constant SYMBOL)' -- a `let'/`let*' binding name
 * that is `t', `nil' or a keyword (lisp_core.c). */
[[noreturn]] void lisp_raise_setting_constant(
    FeContext *context, FeObject *symbol);
/* Raise Emacs' `(end-of-buffer)' / `(beginning-of-buffer)', the two edges
 * a motion or an edit runs into.  Both carry no data, and both became
 * raisable at the Phase 20 fe pin, which is what added them to fe's
 * condition table (lisp_core.c). */
[[noreturn]] void lisp_raise_buffer_edge(FeContext *context, bool at_end);
/* The two type checks a native writes most, raising wrong-type-argument
 * with Emacs' predicate name rather than letting fe's own accessor produce
 * its "expected string, got integer" prose (lisp_core.c).  kg takes a
 * string wherever Emacs takes a symbol for a hook or feature name, so the
 * symbol check accepts both and still reports `symbolp'. */
void lisp_check_string(FeContext *context, FeObject *object);
void lisp_check_symbol_or_string(FeContext *context, FeObject *object);
/* ---- Minibuffer reads (lisp_prompt.c) --------------------------------
 *
 * The four readers `(interactive "s/n/f/F/b/B")` and the public
 * `read-*` forms share.  Everything optional about a read is in one
 * struct rather than in a per-reader argument list, so the two callers
 * cannot drift into different policies:
 *
 *   initial     the prompt's initial text (NULL for the reader's own
 *               default: empty for a string, the current buffer's
 *               directory for a path)
 *   fallback    what an EMPTY answer becomes -- a DEFAULT argument's
 *               value, or NULL/nil for none.  Held across the prompt
 *               only as an element of the native's own argument list,
 *               which the evaluator roots
 *   must_match  the answer must name an existing file (`f`) or an
 *               existing buffer (`b`)
 *   kind        what an overflow diagnostic names this read
 */
struct lisp_read_options {
	const char *initial;
	FeObject *fallback;
	bool must_match;
	const char *kind;
};

/* Raise unless a live command prompt exists to read from: prompting is
 * refused outside a key/M-x command context and while another prompt is
 * already up (07E's re-entrancy rule, which the public forms inherit). */
void lisp_prompt_require(FeContext *context);
FeObject *lisp_read_string_prompt(FeContext *context, int fd,
    const char *prompt, const struct lisp_read_options *opt);
FeObject *lisp_read_number_prompt(FeContext *context, int fd,
    const char *prompt, const struct lisp_read_options *opt);
FeObject *lisp_read_path_prompt(FeContext *context, int fd, const char *prompt,
    const struct lisp_read_options *opt);
FeObject *lisp_read_buffer_prompt(FeContext *context, int fd,
    const char *prompt, const struct lisp_read_options *opt);

/* Latch state.error_kind into state.last_error_kind and disarm it
 * (lisp_core.c).  Called by every seam that has finished handling a
 * completion, so none of them leaves a stale kind behind. */
void lisp_settle_completion(void);

/* ---- Runtime execution context (lisp_obj.c) -------------------------- */

/* Initialise the exec context from the active window (frame entry): the
 * window is the authority for the buffer it shows, so this overwrites that
 * buffer's point-table entry from the window's cursor even if one already
 * existed.  May raise. */
void lisp_exec_enter(FeContext *ctx);
/* Sync the exec buffer's runtime point back to the active window when
 * `sync` and the window still shows that buffer, then drop the frame's
 * buffer selection.  Every other buffer's point-table entry is untouched.
 * Safe on the error path too: no sync, just cleanup. */
void lisp_exec_leave(int sync);
/* Select `b` as the exec buffer.  Reuses `b`'s existing point-table entry
 * if it has one (Emacs does not move point when a buffer is merely
 * selected); creates one at buffer start otherwise.  Never touches a
 * window or buf_current.  May raise. */
void lisp_exec_set_buffer(FeContext *ctx, struct editor_buffer *b);
/* Restore a context saved by value, buffer-local bindings included.  Never
 * raises: the hook path calls it while unwinding an error. */
void lisp_exec_restore(FeContext *ctx, struct kg_lisp_exec_ctx saved);
/* The runtime point marker for the exec buffer.  Valid once the exec
 * context has a buffer, which lisp_exec_enter()/lisp_exec_set_buffer()
 * both guarantee before returning; a handle naming nothing otherwise. */
struct kg_marker_handle lisp_exec_point_marker(void);
/* The two halves of keeping the runtime point and the window's cursor
 * agreeing.  Both are no-ops unless the active window shows the exec
 * buffer, so hidden work never moves a displayed window.  `to_window' is
 * what lisp_exec_leave() does at the end of an evaluation; both are what
 * (command-execute ...) does around a built-in command, which knows only
 * the window. */
void lisp_exec_point_to_window(void);
void lisp_exec_point_from_window(FeContext *ctx);

/* Position/codepoint conversions the modules share (lisp_buffer.c). */
FeDouble lisp_finite(
    FeContext *context, FeObject *object, const char *predicate);
long lisp_decode_char(const char *text, int length, int col);
long lisp_optional_count(FeContext *context, FeObject **arguments);
FeObject *lisp_position(FeContext *context, long offset);
/* The exec buffer, raising "current buffer is dead" when it is gone. */
struct editor_buffer *lisp_exec_buffer(FeContext *context);
/* Flat byte position of the runtime point, raising when its marker is
 * gone (a buffer killed mid-frame). */
size_t lisp_exec_point_byte(FeContext *context);
/* 0-based codepoint offset of the runtime point (raises when gone). */
long lisp_exec_point_char(FeContext *context);
/* Set the runtime point to 0-based codepoint offset `off` of `b`. */
void lisp_exec_goto_char(const struct editor_buffer *b, long off);
/* Codepoint length of `b` (row separators count one each). */
long lisp_buffer_char_length(const struct editor_buffer *b);
/* 0-based codepoint offset of the flat byte position `byte` in `b`. */
long lisp_char_offset_of(const struct editor_buffer *b, size_t byte);
/* (row, byte column) of 0-based codepoint offset `off` in `b`, clamped to
 * the buffer. */
void lisp_rowcol_of_char_offset(
    const struct editor_buffer *b, long off, int *row, int *col);
/* Flat byte position of 0-based codepoint offset `off` in `b`. */
size_t lisp_byte_of_char_offset(const struct editor_buffer *b, long off);
/* A 1-based position argument (as Emacs positions read), clamped to
 * [point-min, point-max] of `b`, as the 0-based codepoint offset the
 * buffer helpers expect. */
long lisp_offset_argument(
    FeContext *context, const struct editor_buffer *b, FeObject *object);

#if KG_PERF_COUNTERS
/* CLOCK_MONOTONIC in nanoseconds (lisp_core.c); counting builds only. */
long long lisp_monotonic_ns(void);
#endif

/* XDG config resolution (lisp_io.c). */
int lisp_config_path(char *out, size_t outsize, const char *stem);
/* True when NAME already ends in ".el" (lisp_io.c).  The one place that
 * question is answered, so `load` and `require` cannot disagree about
 * the same input the way they did before sub-plan 10C. */
bool lisp_has_el_suffix(const char *name, size_t length);
/* Open the Lisp source at PATH as a load stream, honouring
 * LISP_MAX_LOAD_DEPTH: reads the whole file, takes the next
 * state.load_streams slot, and enters an fe input unit labelled with the
 * path (lisp_io.c).  Shared by `load' and `require''s loading arm, whose
 * prelude loops read it back one form at a time.  Returns the slot. */
size_t lisp_load_stream_begin(FeContext *context, const char *path);

/* The command registry (lisp_core.c). */
struct lisp_command *find_lisp_command(const char *name);

/* Native functions, bound into the interpreter by register_natives()
 * (lisp_prelude.c); each lives in the module that owns its concern. */
FeObject *native_message(FeContext *context, FeObject *arguments);
FeObject *native_insert(FeContext *context, FeObject *arguments);
FeObject *native_buffer_name(FeContext *context, FeObject *arguments);
/* The loader's C half (lisp_io.c).  `load' itself is prelude Lisp: a loop
 * of internal--read-form/eval inside the input unit internal--load-begin
 * enters, so a throw, condition or quit out of a loaded form propagates
 * in the CURRENT run to whatever catch or handler encloses the (load ...).
 * resolve turns a load NAME into a path; begin/read-form/end own the
 * stream. */
FeObject *native_internal_resolve_load(FeContext *context, FeObject *arguments);
FeObject *native_internal_load_begin(FeContext *context, FeObject *arguments);
FeObject *native_internal_read_form(FeContext *context, FeObject *arguments);
FeObject *native_internal_load_end(FeContext *context, FeObject *arguments);
FeObject *native_format(FeContext *context, FeObject *arguments);
FeObject *native_point_offset(FeContext *context, FeObject *arguments);
FeObject *native_point_min(FeContext *context, FeObject *arguments);
FeObject *native_point_max(FeContext *context, FeObject *arguments);
FeObject *native_goto_char(FeContext *context, FeObject *arguments);
FeObject *native_goto_line(FeContext *context, FeObject *arguments);
FeObject *native_line_number(FeContext *context, FeObject *arguments);
FeObject *native_current_column(FeContext *context, FeObject *arguments);
FeObject *native_mark(FeContext *context, FeObject *arguments);
FeObject *native_set_mark(FeContext *context, FeObject *arguments);
FeObject *native_deactivate_mark(FeContext *context, FeObject *arguments);
FeObject *native_region_beginning(FeContext *context, FeObject *arguments);
FeObject *native_region_end(FeContext *context, FeObject *arguments);
FeObject *native_buffer_substring(FeContext *context, FeObject *arguments);
FeObject *native_char_after(FeContext *context, FeObject *arguments);
FeObject *native_forward_word(FeContext *context, FeObject *arguments);
/* Line/character motion and character-set skipping (lisp_motion.c). */
FeObject *native_forward_line(FeContext *context, FeObject *arguments);
FeObject *native_forward_char(FeContext *context, FeObject *arguments);
FeObject *native_backward_char(FeContext *context, FeObject *arguments);
FeObject *native_beginning_of_line(FeContext *context, FeObject *arguments);
FeObject *native_end_of_line(FeContext *context, FeObject *arguments);
FeObject *native_skip_chars_forward(FeContext *context, FeObject *arguments);
FeObject *native_skip_chars_backward(FeContext *context, FeObject *arguments);
FeObject *native_backward_word(FeContext *context, FeObject *arguments);
FeObject *native_bounds_of_thing(FeContext *context, FeObject *arguments);
FeObject *native_string_length(FeContext *context, FeObject *arguments);
FeObject *native_substring(FeContext *context, FeObject *arguments);
FeObject *native_concat(FeContext *context, FeObject *arguments);
FeObject *native_string_equal(FeContext *context, FeObject *arguments);
FeObject *native_char_to_string(FeContext *context, FeObject *arguments);
FeObject *native_string_to_char(FeContext *context, FeObject *arguments);
FeObject *native_string_to_number(FeContext *context, FeObject *arguments);
FeObject *native_make_string(FeContext *context, FeObject *arguments);
FeObject *native_upcase(FeContext *context, FeObject *arguments);
FeObject *native_downcase(FeContext *context, FeObject *arguments);
FeObject *native_capitalize(FeContext *context, FeObject *arguments);
/* The three UTF-8 conversions the string and search natives share
 * (lisp_string.c).  All three take a byte length and count whole glyphs,
 * so a caller never lands inside one: `length' answers how many
 * codepoints `text' holds, `byte' the byte offset of the Nth codepoint,
 * and `chars' how many codepoints precede a byte offset.  `byte' and
 * `chars' are inverses on glyph boundaries. */
int lisp_utf8_length(const char *text, int length);
int lisp_utf8_byte(const char *text, int length, int chars);
int lisp_utf8_chars(const char *text, int length, int offset);
FeObject *native_type_of(FeContext *context, FeObject *arguments);
FeObject *native_stringp(FeContext *context, FeObject *arguments);
FeObject *native_symbolp(FeContext *context, FeObject *arguments);
FeObject *native_numberp(FeContext *context, FeObject *arguments);
FeObject *native_consp(FeContext *context, FeObject *arguments);
FeObject *native_listp(FeContext *context, FeObject *arguments);
FeObject *native_functionp(FeContext *context, FeObject *arguments);
FeObject *native_command(FeContext *context, FeObject *arguments);
FeObject *native_commandp(FeContext *context, FeObject *arguments);
FeObject *native_interactive_form(FeContext *context, FeObject *arguments);
FeObject *native_command_names(FeContext *context, FeObject *arguments);
FeObject *native_command_documentation(FeContext *context, FeObject *arguments);
FeObject *native_prefix_numeric_value(FeContext *context, FeObject *arguments);
FeObject *lisp_prefix_object(
    FeContext *context, const struct command_prefix *prefix);
/* Phase 2 of doc/plans/2026-08-14-embedded-prelude.md: the prelude names
 * Phase 0.2 found eager on every startup path, small and ordering-free
 * enough to be natives instead of the lambdas lisp/prelude.el used to
 * carry (lisp_cmd.c). */
FeObject *native_reverse(FeContext *context, FeObject *arguments);
FeObject *native_nconc(FeContext *context, FeObject *arguments);
FeObject *native_internal_bind_name(FeContext *context, FeObject *arguments);
FeObject *native_internal_bind_value(FeContext *context, FeObject *arguments);
FeObject *native_internal_doc_put(FeContext *context, FeObject *arguments);
FeObject *native_internal_variable_doc_put(
    FeContext *context, FeObject *arguments);
/* Read and clear the value the command activation just finished produced
 * (lisp_core.c).  Nothing may allocate between the activation returning and
 * this call -- see the definition. */
FeObject *lisp_take_command_value(FeContext *context);
int64_t lisp_prefix_number(FeContext *context, FeObject *object);
FeObject *native_define_command(FeContext *context, FeObject *arguments);
FeObject *native_remove_command(FeContext *context, FeObject *arguments);
FeObject *native_remove_command_if_present(
    FeContext *context, FeObject *arguments);
FeObject *native_bind_key(FeContext *context, FeObject *arguments);
FeObject *native_unbind_key(FeContext *context, FeObject *arguments);
FeObject *native_current_buffer(FeContext *context, FeObject *arguments);
FeObject *native_buffer_list(FeContext *context, FeObject *arguments);
FeObject *native_get_buffer(FeContext *context, FeObject *arguments);
FeObject *native_get_buffer_create(FeContext *context, FeObject *arguments);
FeObject *native_buffer_live_p(FeContext *context, FeObject *arguments);
FeObject *native_set_buffer(FeContext *context, FeObject *arguments);
FeObject *native_switch_to_buffer(FeContext *context, FeObject *arguments);
FeObject *native_kill_buffer(FeContext *context, FeObject *arguments);
FeObject *native_delete_region(FeContext *context, FeObject *arguments);
FeObject *native_delete_char(FeContext *context, FeObject *arguments);
FeObject *native_erase_buffer(FeContext *context, FeObject *arguments);
FeObject *native_buffer_file_name(FeContext *context, FeObject *arguments);
FeObject *native_buffer_modified_p(FeContext *context, FeObject *arguments);
FeObject *native_set_buffer_modified_p(FeContext *context, FeObject *arguments);
FeObject *native_replace_region(FeContext *context, FeObject *arguments);
FeObject *native_search_forward(FeContext *context, FeObject *arguments);
FeObject *native_search_backward(FeContext *context, FeObject *arguments);
FeObject *native_re_search_forward(FeContext *context, FeObject *arguments);
FeObject *native_re_search_backward(FeContext *context, FeObject *arguments);
FeObject *native_match_beginning(FeContext *context, FeObject *arguments);
FeObject *native_match_end(FeContext *context, FeObject *arguments);
FeObject *native_string_match(FeContext *context, FeObject *arguments);
FeObject *native_regexp_quote(FeContext *context, FeObject *arguments);
FeObject *native_looking_at(FeContext *context, FeObject *arguments);
FeObject *native_make_marker(FeContext *context, FeObject *arguments);
FeObject *native_point_marker(FeContext *context, FeObject *arguments);
FeObject *native_copy_marker(FeContext *context, FeObject *arguments);
FeObject *native_set_marker(FeContext *context, FeObject *arguments);
FeObject *native_marker_position(FeContext *context, FeObject *arguments);
FeObject *native_marker_buffer(FeContext *context, FeObject *arguments);
FeObject *native_excursion_capture(FeContext *context, FeObject *arguments);
FeObject *native_excursion_restore(FeContext *context, FeObject *arguments);
/* Buffer-local variable bindings (lisp_locals.c), Phase 18. */
FeObject *native_internal_set_buffer_local(
    FeContext *context, FeObject *arguments);
FeObject *native_make_local_variable(FeContext *context, FeObject *arguments);
FeObject *native_kill_local_variable(FeContext *context, FeObject *arguments);
FeObject *native_local_variable_p(FeContext *context, FeObject *arguments);
FeObject *native_default_value(FeContext *context, FeObject *arguments);
FeObject *native_set_default(FeContext *context, FeObject *arguments);
FeObject *native_buffer_local_value(FeContext *context, FeObject *arguments);
FeObject *native_add_hook(FeContext *context, FeObject *arguments);
FeObject *native_remove_hook(FeContext *context, FeObject *arguments);
FeObject *native_run_hooks(FeContext *context, FeObject *arguments);
FeObject *native_define_key(FeContext *context, FeObject *arguments);
FeObject *native_lookup_key(FeContext *context, FeObject *arguments);
FeObject *native_current_local_map(FeContext *context, FeObject *arguments);
FeObject *native_start_process(FeContext *context, FeObject *arguments);
FeObject *native_start_shell_command(FeContext *context, FeObject *arguments);
FeObject *native_process_live_p(FeContext *context, FeObject *arguments);
FeObject *native_delete_process(FeContext *context, FeObject *arguments);
FeObject *native_process_buffer(FeContext *context, FeObject *arguments);
FeObject *native_set_process_filter(FeContext *context, FeObject *arguments);
FeObject *native_set_process_sentinel(FeContext *context, FeObject *arguments);
FeObject *native_process_status(FeContext *context, FeObject *arguments);
FeObject *native_provide(FeContext *context, FeObject *arguments);
/* `require''s C half (lisp_require.c); the loop itself is the same
 * prelude loop `load' uses.  resolve answers nil when the feature is
 * already provided, else the resolved path; push/pop own the cyclic-
 * require stack from the prelude's unwind-protect; check is the
 * did-not-provide verdict and the package-load perf accounting. */
FeObject *native_internal_require_resolve(
    FeContext *context, FeObject *arguments);
FeObject *native_internal_require_push(FeContext *context, FeObject *arguments);
FeObject *native_internal_require_pop(FeContext *context, FeObject *arguments);
FeObject *native_internal_require_check(
    FeContext *context, FeObject *arguments);
FeObject *native_featurep(FeContext *context, FeObject *arguments);
FeObject *native_add_to_load_path(FeContext *context, FeObject *arguments);
/* The minibuffer reads (lisp_prompt.c), Phase 16. */
FeObject *native_read_string(FeContext *context, FeObject *arguments);
FeObject *native_read_number(FeContext *context, FeObject *arguments);
FeObject *native_read_file_name(FeContext *context, FeObject *arguments);
FeObject *native_read_buffer(FeContext *context, FeObject *arguments);
FeObject *native_y_or_n_p(FeContext *context, FeObject *arguments);
FeObject *native_yes_or_no_p(FeContext *context, FeObject *arguments);
FeObject *native_completing_read(FeContext *context, FeObject *arguments);

/* Startup (lisp_prelude.c): bind the natives and evaluate the prelude. */
void register_natives(FeContext *context);
void evaluate_prelude(FeContext *context);
/* Phase 1 of doc/plans/2026-08-14-embedded-prelude.md (lisp_prelude.c):
 * install one self-replacing stub per deferred name, and the native each
 * stub's first call uses to force its real definition.  Call
 * install_deferred_stubs() once, after evaluate_prelude() has returned. */
void install_deferred_stubs(FeContext *context);
FeObject *native_internal_force_deferred(
    FeContext *context, FeObject *arguments);

#endif /* KG_LISP_INTERNAL_H */
