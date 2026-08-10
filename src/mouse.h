/* mouse.h -- terminal mouse reporting: the DECSET switches, the SGR
 * report decoder, and what kg does with a decoded report.
 *
 * kg asks the terminal for SGR-encoded mouse reports (DECSET 1000, 1002
 * and 1006) whenever it enters raw mode, and takes the request back
 * whenever it leaves -- exit, C-z, and the resume after it.  A terminal
 * that understands none of the three ignores the request string, which
 * is what makes asking for it by default safe; see
 * kg_mouse_term_reports() for the one case kg does not ask at all.
 *
 * The decoder half is deliberately separate from the acting half.
 * src/tty.c consumes a report the moment it reads one, whichever loop is
 * reading keys, so no part of one can ever reach a buffer as text; only
 * the main loop's keypress step calls kg_mouse_handle_pending(), so a
 * click that arrives while a minibuffer prompt is up is discarded rather
 * than moving point under the prompt.
 *
 * Self-contained, in the shape src/localvars.h has: this header is
 * compiled standalone by `make header-check`.
 */

#ifndef KG_MOUSE_H
#define KG_MOUSE_H

/* One decoded SGR report.  `button` has had the modifier bits taken off,
 * so it is one of the KG_MOUSE_* codes below or another button kg does
 * not act on. */
struct kg_mouse_report {
	int button;
	int col; /* 1-based terminal column */
	int row; /* 1-based terminal row */
	bool motion; /* the drag bit (32) was set */
	bool release; /* the report ended in 'm' rather than 'M' */
};

#define KG_MOUSE_BUTTON_LEFT 0
#define KG_MOUSE_WHEEL_UP 64
#define KG_MOUSE_WHEEL_DOWN 65

/* Longest parameter string kg will read between "CSI <" and the final
 * byte, terminator included.  Three decimal fields and two separators;
 * anything longer is not a report any terminal sends. */
#define KG_MOUSE_PARAMS_MAX 32

/* Whether kg asks a terminal named `term` for mouse reports at all.
 * There is no capability query worth having -- a terminal that does not
 * understand the request ignores it silently -- so this is generous on
 * purpose: only a TERM that is unset, empty, "dumb" or "unknown" is
 * refused, and everything else is asked.  The escape hatch for a
 * terminal that answers badly is the xterm-mouse-mode command, not a
 * longer list here. */
[[nodiscard]] bool kg_mouse_term_reports(const char *term);

/* Ask for / stop asking for reports.  Paired with entering and leaving
 * raw mode (src/tty.c), so C-z's suspend takes the request back and its
 * resume puts it out again.  Both are idempotent. */
void kg_mouse_start(void);
void kg_mouse_stop(void);

/* Whether the xterm-mouse-mode is on -- i.e. whether a decoded report is
 * acted on.  The first call decides it from TERM. */
[[nodiscard]] bool kg_mouse_enabled(void);

/* The xterm-mouse-mode command: flip it, and put the terminal's
 * reporting in step with the new state. */
void kg_mouse_toggle(void);

/* Decode "b;x;y" (the bytes between "CSI <" and `final_byte`, which is 'M'
 * for a press and 'm' for a release).  Returns false for anything that
 * is not exactly three decimal fields; the caller has consumed the
 * report either way. */
[[nodiscard]] bool kg_mouse_parse_sgr(
    const char *params, char final_byte, struct kg_mouse_report *out);

/* Hand the decoder's result over.  One slot is enough: the decoder
 * produces one key event per report, and the main loop consumes that
 * event -- and with it this slot -- before it can read the next one. */
void kg_mouse_record(const char *params, char final_byte);

/* Act on the held report, if there is one and the mode is on. */
void kg_mouse_handle_pending(void);

#endif /* KG_MOUSE_H */
