#ifndef KG_LISP_LOCALS_H
#define KG_LISP_LOCALS_H

/* Buffer-local variable bindings: kg's storage, Emacs' representation.
 *
 * Emacs does not keep a variable's per-buffer values in a table the reader
 * consults.  It keeps ONE value cell per symbol, holding whichever binding
 * is in force in the current buffer, and swaps the displaced value out to
 * the side when the current buffer changes.  kg does the same thing, for
 * the reason the plan gives: the language runtime owns no editor objects,
 * so fe cannot be taught what a buffer is, and an ordinary symbol reference
 * inside fe's evaluator must keep costing exactly one cell read.  Every
 * rule the manifest's phase18-* rows pin follows from that one choice --
 * including what a dynamic `let' binds, since a `let' over the cell binds
 * whichever binding the swap has put there.
 *
 * The values swapped aside live in the value cells of HIDDEN INTERNED
 * SYMBOLS (`internal--local-N', `internal--default-N'), one per table slot,
 * created on demand.  That is not a trick for its own sake: it is what
 * makes the swap allocation-free, and therefore incapable of raising, which
 * it has to be because it runs on error paths (a hook's exec-context
 * restore).  A root would have to be released and re-created on every
 * switch -- one cons each -- and interned symbols are already permanent GC
 * roots by virtue of the interner, so the values they hold need no roots of
 * kg's own.  The names are reachable from Lisp; kg's Lisp is trusted code
 * and writing one corrupts a stashed binding exactly as `setq'ing any other
 * internal would.
 *
 * Nothing here may include fe.h: this header is a standalone header-check
 * unit and Fe is reachable only from src/lisp_*.c implementation files.
 */

#include <stddef.h>
#include <stdint.h>

#include "bufhandle.h"

struct FeContext;
struct FeObject;

/* How many distinct variable names may have a buffer-local binding at
 * once, and how many (name, buffer) bindings may be live at once.  Both
 * are ceilings on a table walked linearly on every buffer switch, so they
 * are small on purpose: kg's own consumer is `fill-column', and an init
 * file that wants sixteen buffer-local names in one session is doing
 * something this editor has no other machinery for.  Overflow is a raise
 * naming the table, never a silent drop. */
#define LISP_MAX_LOCAL_VARS 16
#define LISP_MAX_LOCAL_BINDINGS 64

/* One variable that has at least one buffer-local binding somewhere.  The
 * entry exists exactly while that is true: the last binding to die takes
 * the entry with it, and the name goes back to being an ordinary global.
 *
 * `cell' holds the DEFAULT value, and is authoritative exactly while the
 * currently swapped-in buffer has a binding for this variable -- otherwise
 * the default is in `symbol''s own value cell, where every reader already
 * looks.  src/lisp_locals.c's `default_cell()' is the one function that
 * answers which of the two it is. */
struct kg_lisp_local_var {
	bool active;
	struct FeObject *symbol; /* the user's variable; interned, so stable */
	struct FeObject *cell; /* hidden symbol holding the stashed default */
};

/* One (variable, buffer) binding.  `cell' holds the value while this
 * buffer is NOT the swapped-in one; while it is, the value is in the
 * variable's own cell and `cell' is stale, exactly as Emacs' is.
 *
 * `stamp' names THIS binding, and only this one, for as long as it lives:
 * it is what a live `let' over the binding carries as its fe tag, so a
 * restore can ask whether the storage it displaced still exists.  A slot
 * that is reclaimed and handed out again gets a fresh stamp, which is the
 * whole point -- the old tag then matches nothing and its saved value is
 * dropped, as Emacs drops a binding whose buffer has stopped having one.
 * Never zero (zero is the tag for "the default"), and monotonic: wrapping
 * it needs 2^64 buffer-local bindings to have been created in one
 * session. */
struct kg_lisp_local_binding {
	bool active;
	size_t var; /* index into lisp_locals_table::vars */
	struct kg_buffer_handle buffer;
	struct FeObject *cell;
	uintptr_t stamp;
};

/* `swapped' is which buffer's bindings are in the variables' own value
 * cells right now.  It is deliberately NOT read off the execution context:
 * the context is emptied at frame exit, and emptying it must not disturb
 * the cells (Emacs leaves the last buffer's values in place between
 * commands too).  Everything that changes the current buffer routes
 * through lisp_locals_switch(). */
struct lisp_locals_table {
	struct kg_lisp_local_var vars[LISP_MAX_LOCAL_VARS];
	struct kg_lisp_local_binding bindings[LISP_MAX_LOCAL_BINDINGS];
	struct kg_buffer_handle swapped;
	size_t var_count; /* active entries; zero is the whole fast path */
	uintptr_t stamps; /* how many bindings have ever been created */
};

/* Make `to` the buffer whose bindings are in force: stash the outgoing
 * buffer's values and load the incoming one's, then reclaim every binding
 * whose buffer has died.  A no-op, beyond recording `to`, when no variable
 * has a buffer-local binding at all -- which is the state every kg session
 * that never says `setq-local' stays in.
 *
 * CANNOT RAISE, and callers depend on that: it runs from a hook's
 * exec-context restore, which is an error path.  Everything it does is a
 * value-cell read or write on a symbol it registered itself. */
void lisp_locals_switch(struct FeContext *ctx, struct kg_buffer_handle to);

/* `handle`'s buffer has just been killed from inside a Lisp frame.  When
 * it was the one whose bindings were in force, the value cells still hold
 * values belonging to a buffer that no longer exists, and the rest of the
 * frame would read them; this puts the defaults back.  A kill of any other
 * buffer needs nothing -- the next switch reaps its bindings.  Cannot
 * raise, for lisp_locals_switch()'s reason. */
void lisp_locals_buffer_killed(
    struct FeContext *ctx, struct kg_buffer_handle handle);

/* ---- What a `let' over a buffer-local name displaced ------------------
 *
 * These two are fe's `FeBindingSaveFn'/`FeBindingTargetFn' (FE_API_VERSION
 * 11), registered once in src/lisp_core.c, and they are the whole of the
 * per-binding buffer tag Emacs keeps in its specpdl.  fe's `let' saves the
 * symbol's one value cell; these say which storage that cell BELONGED to
 * when it was saved, and therefore where the saved value goes back.
 *
 * Both are pure reads of the table above and of nothing else -- no
 * allocation, no raise, no evaluation, which is fe's contract for them --
 * and deliberately no re-synchronisation with the execution context: what
 * the value cell holds is what `swapped' says it holds, by definition, so
 * asking anything else would answer about a buffer whose values are not in
 * the cells.
 *
 * lisp_locals_bind_tag() answers zero for "the default was in the cell",
 * which is every binding of every ordinary variable, and the binding's
 * `stamp' when the swapped-in buffer had a local binding of its own.
 *
 * lisp_locals_bind_target() turns that back into a cell: the default's
 * current home for a zero tag (which is Emacs' `set-default' when a
 * `setq-local' inside the form created a binding meanwhile), the symbol's
 * own cell when the tagged binding is the swapped-in one, that binding's
 * stash otherwise, and NULL -- fe's "drop it" -- when the binding is gone
 * or its buffer is dead. */
uintptr_t lisp_locals_bind_tag(struct FeContext *ctx, struct FeObject *symbol);
struct FeObject *lisp_locals_bind_target(
    struct FeContext *ctx, struct FeObject *symbol, uintptr_t tag);
/* Read BUFFER's binding, or its default, without selecting the buffer.
 * Returns nullptr for an unbound value and performs no evaluation. */
struct FeObject *lisp_locals_buffer_value(struct FeContext *ctx,
    struct FeObject *symbol, struct kg_buffer_handle buffer);

#endif /* KG_LISP_LOCALS_H */
