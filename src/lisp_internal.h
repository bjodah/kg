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
#include "lisp_obj.h"
#include "marker.h"
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
	 * detection.  Reset to empty by frame recovery (release_frame_buffers
	 * in lisp_core.c), same treatment load_depth gets, since a longjmp
	 * abandons every nested require the same way it abandons every
	 * nested load. */
	char requiring[LISP_MAX_REQUIRE_STACK][LISP_FEATURE_NAME_MAX];
	size_t requiring_depth;
	bool frame_active;
	struct lisp_prefix_binding *prefix_binding;
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
/* The body-thunk call a *wrapping* native makes (lisp_core.c): one that has
 * already registered its restore with FeProtectWithCleanup and now runs the
 * body between the save and that restore.  Transparent to the enclosing
 * run -- see the definition for why FeCall was not. */
FeObject *lisp_call_body(FeContext *context, FeObject *body);
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
/* The runtime point marker for the exec buffer.  Valid once the exec
 * context has a buffer, which lisp_exec_enter()/lisp_exec_set_buffer()
 * both guarantee before returning; a handle naming nothing otherwise. */
struct kg_marker_handle lisp_exec_point_marker(void);

/* Position/codepoint conversions the modules share (lisp_buffer.c). */
FeDouble lisp_finite(FeContext *context, FeObject *object);
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

/* XDG config resolution (lisp_io.c). */
int lisp_config_path(char *out, size_t outsize, const char *stem);
/* Evaluate the Lisp source at PATH, honouring LISP_MAX_LOAD_DEPTH; shared
 * by native_load and native_require (lisp_io.c). */
void lisp_eval_file(FeContext *context, const char *path);

/* The command registry (lisp_core.c). */
struct lisp_command *find_lisp_command(const char *name);

/* Native functions, bound into the interpreter by register_natives()
 * (lisp_prelude.c); each lives in the module that owns its concern. */
FeObject *native_message(FeContext *context, FeObject *arguments);
FeObject *native_insert(FeContext *context, FeObject *arguments);
FeObject *native_buffer_name(FeContext *context, FeObject *arguments);
FeObject *native_load(FeContext *context, FeObject *arguments);
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
FeObject *native_backward_word(FeContext *context, FeObject *arguments);
FeObject *native_bounds_of_thing(FeContext *context, FeObject *arguments);
FeObject *native_string_length(FeContext *context, FeObject *arguments);
FeObject *native_substring(FeContext *context, FeObject *arguments);
FeObject *native_concat(FeContext *context, FeObject *arguments);
FeObject *native_string_equal(FeContext *context, FeObject *arguments);
FeObject *native_char_to_string(FeContext *context, FeObject *arguments);
FeObject *native_string_to_char(FeContext *context, FeObject *arguments);
FeObject *native_type_of(FeContext *context, FeObject *arguments);
FeObject *native_stringp(FeContext *context, FeObject *arguments);
FeObject *native_symbolp(FeContext *context, FeObject *arguments);
FeObject *native_numberp(FeContext *context, FeObject *arguments);
FeObject *native_consp(FeContext *context, FeObject *arguments);
FeObject *native_functionp(FeContext *context, FeObject *arguments);
FeObject *native_command(FeContext *context, FeObject *arguments);
FeObject *native_prefix_numeric_value(FeContext *context, FeObject *arguments);
FeObject *lisp_prefix_object(FeContext *context, const struct command_prefix *prefix);
int64_t lisp_prefix_number(FeContext *context, FeObject *object);
FeObject *native_define_command(FeContext *context, FeObject *arguments);
FeObject *native_remove_command(FeContext *context, FeObject *arguments);
FeObject *native_remove_command_if_present(FeContext *context, FeObject *arguments);
FeObject *native_bind_key(FeContext *context, FeObject *arguments);
FeObject *native_unbind_key(FeContext *context, FeObject *arguments);
FeObject *native_current_buffer(FeContext *context, FeObject *arguments);
FeObject *native_buffer_list(FeContext *context, FeObject *arguments);
FeObject *native_get_buffer(FeContext *context, FeObject *arguments);
FeObject *native_get_buffer_create(FeContext *context, FeObject *arguments);
FeObject *native_buffer_live_p(FeContext *context, FeObject *arguments);
FeObject *native_set_buffer(FeContext *context, FeObject *arguments);
FeObject *native_kill_buffer(FeContext *context, FeObject *arguments);
FeObject *native_delete_region(FeContext *context, FeObject *arguments);
FeObject *native_replace_region(FeContext *context, FeObject *arguments);
FeObject *native_search_forward(FeContext *context, FeObject *arguments);
FeObject *native_search_backward(FeContext *context, FeObject *arguments);
FeObject *native_re_search_forward(FeContext *context, FeObject *arguments);
FeObject *native_re_search_backward(FeContext *context, FeObject *arguments);
FeObject *native_match_beginning(FeContext *context, FeObject *arguments);
FeObject *native_match_end(FeContext *context, FeObject *arguments);
FeObject *native_make_marker(FeContext *context, FeObject *arguments);
FeObject *native_set_marker(FeContext *context, FeObject *arguments);
FeObject *native_marker_position(FeContext *context, FeObject *arguments);
FeObject *native_marker_buffer(FeContext *context, FeObject *arguments);
FeObject *native_save_excursion(FeContext *context, FeObject *arguments);
FeObject *native_with_current_buffer(FeContext *context, FeObject *arguments);
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
FeObject *native_require(FeContext *context, FeObject *arguments);
FeObject *native_featurep(FeContext *context, FeObject *arguments);
FeObject *native_add_to_load_path(FeContext *context, FeObject *arguments);

/* Startup (lisp_prelude.c): bind the natives and evaluate the prelude. */
void register_natives(FeContext *context);
void evaluate_prelude(FeContext *context);

#endif /* KG_LISP_INTERNAL_H */
