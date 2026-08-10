/* next_error.h — which results store M-g n walks
 *
 * next-error and previous-error were compilation's alone: the keys, the
 * commands and the store they stepped through all lived in
 * src/compile_nav.c.  Occur is the second store, so the pair needs what
 * Emacs calls `next-error-last-buffer`: the rule that the command which
 * most recently produced results owns the two keys until another one
 * does.
 *
 * That rule is the whole of this module.  It holds no results, no cursor
 * and no idea what a result IS.  A store registers one function -- "move
 * `direction` results and show me where I land" -- and takes the keys with
 * it; the prefix argument, the clamped-at-the-ends message and the visit
 * itself stay in the store that knows what it is stepping through, which
 * is what keeps compilation's behaviour exactly what it was.
 *
 * compile_nav.c registers at install time (init_editor()), so a kg that
 * has run nothing answers M-g n the way it always did, and every compile
 * registers again; occur registers on every run that found something.
 */

#ifndef KG_NEXT_ERROR_H
#define KG_NEXT_ERROR_H

/* A store next-error can walk.  `move` is called with +1 for next-error
 * and -1 for previous-error -- the raw direction, not a step count: a
 * numeric argument means whatever the store says it means, and both
 * stores read it themselves.  `name` names the store in a message and in
 * the test that asserts who owns the keys; nothing branches on it. */
struct next_error_source {
	const char *name;
	void (*move)(int direction);
};

/* Take the keys.  The pointer is stored, not copied, so it must outlive
 * the call -- every caller registers a file-scope constant. */
void next_error_set_source(const struct next_error_source *source);

/* M-g n / M-g M-n and M-g p / M-g M-p, bound through the keymap and named
 * in the command table (`next-error`, `previous-error`). */
void editor_next_error(int fd);
void editor_previous_error(int fd);

/* Who owns the keys right now, or NULL before anything has registered.
 * Test-only introspection: this is how a test asserts that a compile run
 * after an occur takes the keys back, without driving a real jump. */
const char *next_error_current_source_name(void);

#endif /* KG_NEXT_ERROR_H */
