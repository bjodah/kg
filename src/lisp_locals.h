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

#include <stdbool.h>
#include <stddef.h>

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
 * looks.  `lisp_locals_default_cell()' is the one function that answers
 * which of the two it is. */
struct kg_lisp_local_var {
	bool active;
	struct FeObject *symbol; /* the user's variable; interned, so stable */
	struct FeObject *cell; /* hidden symbol holding the stashed default */
};

/* One (variable, buffer) binding.  `cell' holds the value while this
 * buffer is NOT the swapped-in one; while it is, the value is in the
 * variable's own cell and `cell' is stale, exactly as Emacs' is. */
struct kg_lisp_local_binding {
	bool active;
	size_t var; /* index into lisp_locals_table::vars */
	struct kg_buffer_handle buffer;
	struct FeObject *cell;
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

#endif /* KG_LISP_LOCALS_H */
