#include <stddef.h>
#include <stdio.h>

#include "../fe/fe.h"
#include "bufhandle.h"
#include "lisp_internal.h"
#include "lisp_locals.h"
#include "lisp_obj.h"

/* ---- Buffer-local variable bindings -----------------------------------
 * See lisp_locals.h for the representation and for why it is Emacs' own.
 * This file is the whole of it: the swap, the table bookkeeping, and the
 * six natives plus the one internal that `setq-local'/`setq-default'
 * expand into. */

static bool handle_eq(struct kg_buffer_handle a, struct kg_buffer_handle b)
{
	return a.slot == b.slot && a.id == b.id && a.generation == b.generation;
}

/* The hidden symbol holding slot `index` of `kind`'s stash.  Interned, so
 * it is a permanent GC root and the value it holds needs none of kg's own;
 * created once and reused when the slot is reallocated, so a session that
 * cycles bindings does not intern a fresh name each time. */
static FeObject *stash_cell(FeContext *ctx, const char *kind, size_t index)
{
	char name[64];

	(void)snprintf(name, sizeof(name), "internal--%s-%zu", kind, index);
	return FeMakeSymbol(ctx, name);
}

/* Read a stash cell or a variable's own cell, and write one back.  These
 * two are the only places the "no value at all" case is spelled: fe
 * answers a null pointer for an unbound name rather than nil, and the
 * distinction has to survive being stashed aside and put back, or
 * `make-local-variable' on a name with no default would invent one. */
static FeObject *cell_read(FeContext *ctx, FeObject *symbol)
{
	return FeGetValue(ctx, symbol);
}

static void cell_write(FeContext *ctx, FeObject *symbol, FeObject *value)
{
	if (value == nullptr) {
		FeMakeUnbound(ctx, symbol);
	} else {
		FeSet(ctx, symbol, value);
	}
}

static struct kg_lisp_local_var *var_find(FeObject *symbol)
{
	size_t i;

	for (i = 0; i < LISP_MAX_LOCAL_VARS; i++) {
		struct kg_lisp_local_var *v = &state.locals.vars[i];

		if (v->active && v->symbol == symbol) {
			return v;
		}
	}
	return nullptr;
}

static struct kg_lisp_local_binding *binding_find(
    const struct kg_lisp_local_var *v, struct kg_buffer_handle buffer)
{
	size_t index = (size_t)(v - state.locals.vars);
	size_t i;

	for (i = 0; i < LISP_MAX_LOCAL_BINDINGS; i++) {
		struct kg_lisp_local_binding *b = &state.locals.bindings[i];

		if (b->active && b->var == index
		    && handle_eq(b->buffer, buffer)) {
			return b;
		}
	}
	return nullptr;
}

/* The binding `stamp` names, or NULL once it has been killed, reclaimed or
 * has died with its buffer.  A stamp is never reused, so "not found" and
 * "found something else" are the same answer here and the caller does not
 * have to tell them apart. */
static struct kg_lisp_local_binding *binding_by_stamp(uintptr_t stamp)
{
	size_t i;

	for (i = 0; i < LISP_MAX_LOCAL_BINDINGS; i++) {
		struct kg_lisp_local_binding *b = &state.locals.bindings[i];

		if (b->active && b->stamp == stamp) {
			return b;
		}
	}
	return nullptr;
}

/* Where this variable's DEFAULT lives right now: in its stash cell while
 * the swapped-in buffer has a binding of its own, and in the variable's
 * own value cell otherwise -- which is the ordinary global case, and the
 * only one a name with no buffer-local binding anywhere ever has. */
static FeObject *default_cell(FeObject *symbol)
{
	struct kg_lisp_local_var *v = var_find(symbol);

	if (v == nullptr || binding_find(v, state.locals.swapped) == nullptr) {
		return symbol;
	}
	return v->cell;
}

/* ---- The swap ---------------------------------------------------------
 * One variable's half of a buffer switch: put what is in the value cell
 * where the outgoing buffer keeps its values, then load the incoming
 * buffer's.  `writeback` is false when the outgoing buffer has died --
 * there is nothing left to write back TO, and Emacs' answer for a buffer
 * killed underneath a binding is that the binding simply went with it. */
static void swap_one(FeContext *ctx, struct kg_lisp_local_var *v,
    struct kg_lisp_local_binding *from, struct kg_lisp_local_binding *to,
    bool writeback)
{
	if (from == nullptr && to == nullptr) {
		/* Neither buffer has a binding, so the default is in the
		 * value cell and stays there.  Skipping is not just an
		 * optimisation: it is what keeps the stash cell untouched
		 * (and therefore irrelevant) for exactly as long as
		 * default_cell() says it is. */
		return;
	}
	if (writeback) {
		cell_write(ctx, from != nullptr ? from->cell : v->cell,
		    cell_read(ctx, v->symbol));
	}
	cell_write(
	    ctx, v->symbol, cell_read(ctx, to != nullptr ? to->cell : v->cell));
}

/* Reclaim every binding whose buffer no longer resolves, and every
 * variable entry the reclaim left with no bindings at all.  A variable
 * with no bindings is an ordinary global again: its default is back in its
 * own value cell (the swap above put it there), so dropping the entry
 * loses nothing. */
static void reap(void)
{
	size_t i;

	for (i = 0; i < LISP_MAX_LOCAL_BINDINGS; i++) {
		struct kg_lisp_local_binding *b = &state.locals.bindings[i];

		if (b->active && buf_resolve(b->buffer) == nullptr) {
			b->active = false;
		}
	}
	for (i = 0; i < LISP_MAX_LOCAL_VARS; i++) {
		struct kg_lisp_local_var *v = &state.locals.vars[i];
		size_t j;
		bool used = false;

		if (!v->active) {
			continue;
		}
		for (j = 0; j < LISP_MAX_LOCAL_BINDINGS; j++) {
			struct kg_lisp_local_binding *b
			    = &state.locals.bindings[j];

			if (b->active && b->var == i) {
				used = true;
				break;
			}
		}
		if (!used) {
			v->active = false;
			state.locals.var_count--;
		}
	}
}

void lisp_locals_switch(FeContext *ctx, struct kg_buffer_handle to)
{
	bool writeback;
	size_t i;

	if (state.locals.var_count == 0) {
		state.locals.swapped = to;
		return;
	}
	if (handle_eq(to, state.locals.swapped)) {
		return;
	}
	writeback = buf_resolve(state.locals.swapped) != nullptr;
	for (i = 0; i < LISP_MAX_LOCAL_VARS; i++) {
		struct kg_lisp_local_var *v = &state.locals.vars[i];

		if (!v->active) {
			continue;
		}
		swap_one(ctx, v, binding_find(v, state.locals.swapped),
		    binding_find(v, to), writeback);
	}
	reap();
	state.locals.swapped = to;
}

void lisp_locals_buffer_killed(FeContext *ctx, struct kg_buffer_handle handle)
{
	if (handle_eq(handle, state.locals.swapped)) {
		lisp_locals_switch(ctx, (struct kg_buffer_handle) { -1, 0, 0 });
	}
}

/* ---- Table growth -----------------------------------------------------
 * Both of these may raise -- they run from a native, never from the swap
 * -- and both refuse rather than dropping anything on the floor. */

static struct kg_lisp_local_var *var_intern(FeContext *ctx, FeObject *symbol)
{
	struct kg_lisp_local_var *v = var_find(symbol);
	size_t i;

	if (v != nullptr) {
		return v;
	}
	for (i = 0; i < LISP_MAX_LOCAL_VARS; i++) {
		v = &state.locals.vars[i];
		if (!v->active) {
			v->cell = stash_cell(ctx, "default", i);
			v->symbol = symbol;
			v->active = true;
			state.locals.var_count++;
			return v;
		}
	}
	FeHandleError(ctx, "too many buffer-local variables");
}

/* This buffer's binding for `v`, creating one if it has none.  Creating it
 * is the moment the default moves out of the way: the value cell holds the
 * default right now (nothing is swapped in for this variable in this
 * buffer), so it is stashed first and the cell is left holding it, which
 * is exactly `make-local-variable''s "seeded from the default". */
static struct kg_lisp_local_binding *binding_intern(
    FeContext *ctx, struct kg_lisp_local_var *v)
{
	struct kg_lisp_local_binding *b = binding_find(v, state.locals.swapped);
	size_t i;

	if (b != nullptr) {
		return b;
	}
	for (i = 0; i < LISP_MAX_LOCAL_BINDINGS; i++) {
		b = &state.locals.bindings[i];
		if (b->active) {
			continue;
		}
		b->cell = stash_cell(ctx, "local", i);
		b->var = (size_t)(v - state.locals.vars);
		b->buffer = state.locals.swapped;
		b->stamp = ++state.locals.stamps;
		b->active = true;
		cell_write(ctx, v->cell, cell_read(ctx, v->symbol));
		return b;
	}
	FeHandleError(ctx, "too many buffer-local bindings");
}

/* ---- The buffer tag on a `let' ----------------------------------------
 * See lisp_locals.h for what these two answer and why they read nothing but
 * the table.  They are fe's binding hooks, so they run at every dynamic
 * bind and every dynamic restore in the editor, including the ones that
 * have nothing to do with buffers: the first test in each is the one that
 * makes those cost a load and a compare. */

uintptr_t lisp_locals_bind_tag(FeContext *ctx, FeObject *symbol)
{
	struct kg_lisp_local_var *v;
	struct kg_lisp_local_binding *b;

	(void)ctx;
	if (state.locals.var_count == 0) {
		return 0;
	}
	v = var_find(symbol);
	b = v != nullptr ? binding_find(v, state.locals.swapped) : nullptr;
	return b != nullptr ? b->stamp : 0;
}

FeObject *lisp_locals_bind_target(
    FeContext *ctx, FeObject *symbol, uintptr_t tag)
{
	struct kg_lisp_local_binding *b;

	(void)ctx;
	if (tag == 0) {
		return default_cell(symbol);
	}
	b = binding_by_stamp(tag);
	if (b == nullptr || buf_resolve(b->buffer) == nullptr) {
		return nullptr;
	}
	return handle_eq(b->buffer, state.locals.swapped) ? symbol : b->cell;
}

/* ---- Natives ----------------------------------------------------------
 * Every one of them starts by making sure the swap agrees with the
 * execution context.  set-buffer and frame entry already do that, so this
 * is normally a handle comparison; it is here because a native that
 * answered about the wrong buffer would be the one bug in this module
 * nothing else could catch. */

static void sync_swap(FeContext *ctx)
{
	struct editor_buffer *b = buf_resolve(state.exec.buffer);

	/* Tolerant of a dead execution context on purpose.  Killing the
	 * current buffer from Lisp leaves its handle stale for the rest of
	 * the frame (src/lisp_obj.c's kill-buffer comment), and the READERS
	 * below still have an honest answer for that: the kill already put
	 * the defaults back, so `default-value' and friends report them.
	 * The three natives that need a buffer to bind IN ask for it
	 * themselves, and get "current buffer is dead" as any other native
	 * would. */
	if (b != nullptr) {
		lisp_locals_switch(ctx, state.exec.buffer);
	}
}

static FeObject *symbol_argument(FeContext *ctx, FeObject *object)
{
	if (FeGetType(object) != FeTSymbol) {
		lisp_raise_wrong_type(ctx, "symbolp", object);
	}
	return object;
}

/* The buffer a BUFFER-OR-NAME argument names, defaulting to the current
 * one when it is nil or absent.  A non-buffer is `(wrong-type-argument
 * bufferp X)`, which is what lisp_buffer_resolve() raises. */
static struct kg_buffer_handle buffer_argument(FeContext *ctx, FeObject *object)
{
	if (object == nullptr || FeIsNil(object)) {
		return buf_handle_of(lisp_exec_buffer(ctx));
	}
	return buf_handle_of(
	    lisp_buffer_resolve(ctx, object, "buffer-local-value"));
}

/* (internal--set-buffer-local SYMBOL VALUE): what `setq-local' expands to
 * once per pair.  Creates the binding if this buffer has none, then writes
 * the value cell -- which IS this buffer's binding while this buffer is
 * the swapped-in one, so there is nothing else to write. */
FeObject *native_internal_set_buffer_local(FeContext *context, FeObject *args)
{
	FeObject *symbol
	    = symbol_argument(context, FeGetNextArgument(context, &args));
	FeObject *value = FeGetNextArgument(context, &args);

	FeRequireNoArguments(context, args);
	(void)lisp_exec_buffer(context);
	sync_swap(context);
	(void)binding_intern(context, var_intern(context, symbol));
	FeSet(context, symbol, value);
	return value;
}

/* (make-local-variable SYMBOL): give this buffer a binding of its own,
 * seeded from the default, and answer SYMBOL as Emacs does.  Already
 * having one is not an error. */
FeObject *native_make_local_variable(FeContext *context, FeObject *args)
{
	FeObject *symbol
	    = symbol_argument(context, FeGetNextArgument(context, &args));

	FeRequireNoArguments(context, args);
	(void)lisp_exec_buffer(context);
	sync_swap(context);
	(void)binding_intern(context, var_intern(context, symbol));
	return symbol;
}

/* (kill-local-variable SYMBOL): drop this buffer's binding so the default
 * becomes visible again, and answer SYMBOL.  A buffer with no binding is
 * not an error -- there is simply nothing to drop. */
FeObject *native_kill_local_variable(FeContext *context, FeObject *args)
{
	FeObject *symbol
	    = symbol_argument(context, FeGetNextArgument(context, &args));
	struct kg_lisp_local_var *v;
	struct kg_lisp_local_binding *b;

	FeRequireNoArguments(context, args);
	(void)lisp_exec_buffer(context);
	sync_swap(context);
	v = var_find(symbol);
	if (v == nullptr) {
		return symbol;
	}
	b = binding_find(v, state.locals.swapped);
	if (b == nullptr) {
		return symbol;
	}
	/* The stash holds the default; put it back where every reader looks,
	 * then drop the binding.  reap() retires the variable entry too when
	 * this was its last one. */
	cell_write(context, symbol, cell_read(context, v->cell));
	b->active = false;
	reap();
	return symbol;
}

/* (local-variable-p SYMBOL &optional BUFFER) */
FeObject *native_local_variable_p(FeContext *context, FeObject *args)
{
	FeObject *symbol
	    = symbol_argument(context, FeGetNextArgument(context, &args));
	FeObject *buffer = nullptr;
	struct kg_lisp_local_var *v;

	if (!FeIsNil(args)) {
		buffer = FeGetNextArgument(context, &args);
	}
	FeRequireNoArguments(context, args);
	sync_swap(context);
	v = var_find(symbol);
	return FeMakeBool(context,
	    v != nullptr
		&& binding_find(v, buffer_argument(context, buffer))
		    != nullptr);
}

/* (default-value SYMBOL): the value a buffer with no binding of its own
 * sees.  void-variable when there is none, exactly as a reference would
 * be -- not nil. */
FeObject *native_default_value(FeContext *context, FeObject *args)
{
	FeObject *symbol
	    = symbol_argument(context, FeGetNextArgument(context, &args));
	FeObject *value;

	FeRequireNoArguments(context, args);
	sync_swap(context);
	value = cell_read(context, default_cell(symbol));
	if (value == nullptr) {
		lisp_raise_void_variable(context, symbol);
	}
	return value;
}

/* (set-default SYMBOL VALUE): what `setq-default' expands to.  Writes the
 * default wherever it currently lives, which is the variable's own cell
 * unless this buffer has a binding shadowing it. */
FeObject *native_set_default(FeContext *context, FeObject *args)
{
	FeObject *symbol
	    = symbol_argument(context, FeGetNextArgument(context, &args));
	FeObject *value = FeGetNextArgument(context, &args);

	FeRequireNoArguments(context, args);
	sync_swap(context);
	FeSet(context, default_cell(symbol), value);
	return value;
}

/* (buffer-local-value SYMBOL BUFFER): read another buffer's binding
 * without selecting it, falling through to the default when it has none.
 * The swapped-in buffer is asked through the value cell rather than
 * through its stash, because the cell is the live one -- which is also why
 * a `let' in force over that buffer's binding is what this reports, as
 * Emacs' does. */
FeObject *native_buffer_local_value(FeContext *context, FeObject *args)
{
	FeObject *symbol
	    = symbol_argument(context, FeGetNextArgument(context, &args));
	FeObject *buffer = FeGetNextArgument(context, &args);
	struct kg_buffer_handle handle;
	struct kg_lisp_local_var *v;
	struct kg_lisp_local_binding *b;
	FeObject *value;

	FeRequireNoArguments(context, args);
	sync_swap(context);
	handle = buf_handle_of(
	    lisp_buffer_resolve(context, buffer, "buffer-local-value"));
	v = var_find(symbol);
	b = v != nullptr ? binding_find(v, handle) : nullptr;
	if (b == nullptr) {
		value = cell_read(context, default_cell(symbol));
	} else if (handle_eq(handle, state.locals.swapped)) {
		value = cell_read(context, symbol);
	} else {
		value = cell_read(context, b->cell);
	}
	if (value == nullptr) {
		lisp_raise_void_variable(context, symbol);
	}
	return value;
}
