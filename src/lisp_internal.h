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

/* Adapter limits, shared by struct lisp_state and the modules that walk
 * its tables.  Macros rather than constexpr because struct lisp_state's
 * array bounds, and every translation unit that includes the header,
 * need the value. */
#define LISP_MAX_LOAD_DEPTH 8
#define LISP_MAX_COMMANDS 32
#define LISP_COMMAND_NAME_MAX 64

/* A Lisp-defined command: the function object stays alive through its
 * root for the registration lifetime and is released on redefinition or
 * removal. */
struct lisp_command {
	char name[LISP_COMMAND_NAME_MAX]; /* empty marks a free slot */
	struct FeRoot *root;
};

struct lisp_frame {
	jmp_buf error_jump;
	size_t gc_checkpoint;
};

struct lisp_state {
	void *arena;
	FeContext *context;
	struct lisp_frame frame;
	char error[1024];
	int (*interrupt_check)(void);
	/* Source buffers owned by in-flight (load ...) calls.  Fe errors
	 * longjmp past the natives, so frame recovery frees the leftovers. */
	char *load_buffers[LISP_MAX_LOAD_DEPTH];
	size_t load_depth;
	/* Buffer text extracted by a native that has not handed it to Fe
	 * yet; freed by frame recovery for the same reason. */
	char *scratch;
	struct lisp_command commands[LISP_MAX_COMMANDS];
	bool frame_active;
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

/* Position/codepoint conversions the modules share (lisp_buffer.c). */
FeDouble lisp_finite(FeContext *context, FeObject *object);
long lisp_decode_char(const char *text, int length, int col);
long lisp_optional_count(FeContext *context, FeObject **arguments);
FeObject *lisp_position(FeContext *context, long offset);

/* XDG config resolution (lisp_io.c). */
int lisp_config_path(char *out, size_t outsize, const char *stem);

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
FeObject *native_define_command(FeContext *context, FeObject *arguments);
FeObject *native_remove_command(FeContext *context, FeObject *arguments);
FeObject *native_bind_key(FeContext *context, FeObject *arguments);
FeObject *native_unbind_key(FeContext *context, FeObject *arguments);

/* Startup (lisp_prelude.c): bind the natives and evaluate the prelude. */
void register_natives(FeContext *context);
void evaluate_prelude(FeContext *context);

#endif /* KG_LISP_INTERNAL_H */
