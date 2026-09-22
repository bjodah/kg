/* ============================ Numeric arguments ===========================
 *
 * See prefixarg.h. */

#include <string.h>

#include "keyevent.h"
#include "prefixarg.h"

static int prefix_arg_mul_add(int value, int mul, int add)
{
	if (value > (PREFIX_ARG_MAX - add) / mul) {
		return PREFIX_ARG_MAX;
	}
	return value * mul + add;
}

/* M-0..M-9 start a numeric argument by themselves.  A plain digit only
 * continues one that C-u or a Meta digit already started, so these are
 * two separate checks. */
int prefix_meta_digit(struct key_event c)
{
	if ((c.mods & KEY_MOD_META) && c.base >= '0' && c.base <= '9') {
		return (int)(c.base - '0');
	}
	return -1;
}

static int prefix_digit(struct key_event c)
{
	if (c.mods == 0 && c.base >= '0' && c.base <= '9') {
		return (int)(c.base - '0');
	}
	return prefix_meta_digit(c);
}

void prefix_accum_clear(struct prefix_accum *p) { memset(p, 0, sizeof(*p)); }

static enum prefix_step prefix_start(struct prefix_accum *p, struct key_event c)
{
	int meta = prefix_meta_digit(c);

	if (!KEY_IS(c, 'u', KEY_MOD_CTRL) && meta < 0
	    && !KEY_IS(c, '-', KEY_MOD_META)) {
		return PREFIX_STEP_NONE;
	}
	p->pending = 1;
	p->supplied = 1;
	p->raw_kind = KEY_IS(c, '-', KEY_MOD_META)
	    ? PREFIX_RAW_MINUS
	    : (meta < 0 ? PREFIX_RAW_UNIVERSAL : PREFIX_RAW_INTEGER);
	p->universal_count = p->raw_kind == PREFIX_RAW_UNIVERSAL ? 1 : 0;
	/* Three starts, three effective values: bare M-- is -1, a Meta digit
	 * is that digit, and C-u is 4.  Written as one nested ternary the
	 * M-- arm was unreachable -- `meta` is -1 for M-- too, so the outer
	 * test took the C-u branch and M-- C-f moved four characters
	 * *forward*. */
	if (p->raw_kind == PREFIX_RAW_MINUS) {
		p->arg = -1;
	} else {
		p->arg = meta < 0 ? 4 : meta;
	}
	p->no_digits = p->raw_kind == PREFIX_RAW_UNIVERSAL;
	return PREFIX_STEP_TAKEN;
}

static void prefix_add_digit(struct prefix_accum *p, int digit)
{
	if (p->raw_kind == PREFIX_RAW_MINUS) {
		p->raw_kind = PREFIX_RAW_INTEGER;
		p->arg = -digit;
	} else if (p->arg < 0) {
		p->arg = -prefix_arg_mul_add(-p->arg, 10, digit);
	} else {
		p->raw_kind = PREFIX_RAW_INTEGER;
		p->arg = p->no_digits ? digit
				      : prefix_arg_mul_add(p->arg, 10, digit);
	}
	p->no_digits = 0;
}

enum prefix_step prefix_accum_feed(struct prefix_accum *p, struct key_event c)
{
	int digit;

	if (!p->pending) {
		return prefix_start(p, c);
	}
	if (KEY_IS(c, 'u', KEY_MOD_CTRL)) {
		p->raw_kind = PREFIX_RAW_UNIVERSAL;
		p->universal_count++;
		p->arg = prefix_arg_mul_add(p->arg, 4, 0);
		return PREFIX_STEP_TAKEN;
	}
	digit = prefix_digit(c);
	if (digit >= 0) {
		prefix_add_digit(p, digit);
		return PREFIX_STEP_TAKEN;
	}
	/* This key ends the argument and is the command it applies to, so
	 * the accumulated prefix is *committed*, not discarded: the caller
	 * takes supplied/arg/raw_kind and clears them.  Clearing raw_kind
	 * here instead made every Lisp command see a nil raw prefix -- P
	 * nil, p 1 -- however the user spelled the argument. */
	p->pending = 0;
	p->no_digits = 0;
	return PREFIX_STEP_ENDS;
}
