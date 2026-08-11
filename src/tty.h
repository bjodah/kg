#ifndef KG_TTY_H
#define KG_TTY_H

/* tty.h -- the editor's idle wait.
 *
 * Only the wait lives here.  The rest of src/tty.c's declarations are
 * still in src/def.h with the other core ones, and moving them is a
 * separate change; what is here is what a caller outside tty.c needs to
 * name, and it is self-contained so `make header-check` can compile it on
 * its own.
 *
 * What the wait is for.  The editor spends its life waiting for a key,
 * and it used to do that by reading the terminal with VMIN=0/VTIME=1 --
 * a read that ends on the first byte or after 100 ms, whichever comes
 * first.  Everything else the editor does while idle (the auto-revert
 * poll, a running compilation, the process table, the language servers)
 * therefore happened once per 100 ms timeout, because a timeout was the
 * only thing that could interrupt the wait.
 *
 * That is fine for a clock-driven poller and wrong for a pipe.  A
 * language server writing its banner fills its pipe, and one pipe-load
 * per 100 ms tick was the whole of what the editor could take from it:
 * 512 KiB of pre-announce log cost six seconds before kg could connect
 * (doc/plans/2026-08-08-lsp.md, and the input-loop follow-up recorded in
 * commit 706030d).  So the wait now includes those descriptors, and ends
 * early when one of them is ready.
 *
 * The tick did not change and neither did anything hanging off it: the
 * periodic pollers still run once per KG_IDLE_TICK_MS, a descriptor only
 * ever ends a wait EARLY, and the terminal is read by the same read()
 * with the same VTIME as before -- which is what keeps the escape-merge
 * window and paste detection (src/kbd.c) measuring what they always
 * measured.
 */

/* How long a wait may last before the periodic pollers are due again.
 * The VTIME=1 the terminal is put in raw mode with, in milliseconds,
 * because that is the cadence this replaced. */
#define KG_IDLE_TICK_MS 100

/* Why a wait ended. */
enum kg_idle_wake {
	KG_IDLE_KEY, /* the terminal has a byte to read */
	KG_IDLE_FD, /* a watched descriptor is ready */
	KG_IDLE_TICK, /* neither was; the periodic pollers are due */
	KG_IDLE_SIGNAL, /* a signal interrupted the wait */
	KG_IDLE_HANGUP, /* the terminal is gone */
};

/* Wait until `fd` has a byte, one of `count` descriptors in `extra` is
 * ready, or `timeout_ms` passes -- whichever happens first.  Nothing is
 * read here; the answer says who to ask.
 *
 * KG_IDLE_KEY beats KG_IDLE_FD when both are ready, because a user
 * waiting on their own keystroke is the one thing an editor may not make
 * wait.  A terminal that reports anything other than readability has hung
 * up: it would be ready forever without ever yielding a byte, so it is
 * reported as KG_IDLE_HANGUP rather than polled again.
 *
 * Exported for test/test_perf.c, which drives it over a pipe to pin the
 * counters (KG_PERF_IDLE_*) that say a pipe-load costs a wake rather than
 * a tick.  The editor's own caller is read_key_byte()'s idle path. */
enum kg_idle_wake kg_idle_wait(
    int fd, const int *extra, int count, int timeout_ms);

#endif /* KG_TTY_H */
