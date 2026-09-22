#ifndef KG_PREFIXARG_H
#define KG_PREFIXARG_H

/* The numeric-argument accumulator: C-u, C-u C-u, C-u DIGITS, M-DIGITS,
 * M-- and M-- DIGITS, folded key by key into a count and its raw form.
 * The top-level key dispatcher (kbd.c, editor.uarg) and the minibuffer's
 * repeat count (minibuf.c) both run their keys through this one parser.
 * The accumulator only parses: echoing and cancelling stay with the
 * caller, and so does what the finished count means. */

#include "cmd.h"

struct key_event;

/* Every count is capped here.  command_prefix.universal_count keeps what
 * the cap loses for C-u C-u ... */
#define PREFIX_ARG_MAX 1000

struct prefix_accum {
	int pending; /* An argument is being typed. */
	int supplied; /* One was typed; stays set after the argument ends. */
	int arg; /* The effective count so far. */
	int no_digits; /* Between C-u and its first digit: a digit replaces 4.
			*/
	enum prefix_raw_kind raw_kind; /* The raw form so far. */
	int universal_count; /* Bare C-u presses; see command_prefix. */
};

enum prefix_step {
	PREFIX_STEP_NONE, /* Not an argument key, and none was pending. */
	PREFIX_STEP_TAKEN, /* Started or extended the argument. */
	PREFIX_STEP_ENDS, /* Ended the pending argument; it applies to this key.
			   */
};

/* Feed one key.  On PREFIX_STEP_ENDS, `pending` is cleared and `arg`,
 * `supplied`, `raw_kind` and `universal_count` hold the finished
 * argument for the caller to take and clear. */
enum prefix_step prefix_accum_feed(struct prefix_accum *p, struct key_event c);
void prefix_accum_clear(struct prefix_accum *p);
/* The digit of M-0..M-9, else -1. */
int prefix_meta_digit(struct key_event c);

#endif /* KG_PREFIXARG_H */
