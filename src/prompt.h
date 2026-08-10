#ifndef KG_PROMPT_H
#define KG_PROMPT_H

/* Two minibuffer questions the editor's own commands do not ask, but
 * Lisp does: a completing read over a caller-supplied list of candidates
 * (`completing-read'), and a y/n question whose cancellation is
 * distinguishable from a "no" (`y-or-n-p', where C-g is Emacs' `quit'
 * rather than an answer).
 *
 * The candidate reader is the ido-style picker kg already shows for M-x,
 * C-x b and C-x C-f -- editor_picker_filter()/_render()/_emphasise() from
 * path.c are the shared policy -- driven by an array instead of by a
 * table the editor owns.  Kept out of bufmgr.c so the buffer manager does
 * not grow a fourth picker, and out of the Lisp adapter so nothing in
 * src/lisp_*.c has to read keys.
 *
 * Self-contained: bufmgr.h is where `enum minibuf_result' lives, and
 * every reader in kg answers with it. */

#include "bufmgr.h"

/* The most candidates prompt_read_choice() accepts.  Equal to
 * PICKER_MAX_ENTRIES (def.h), which is how many the echo area's pick-list
 * can hold: a candidate the picker could never show or cycle to is not a
 * candidate, so the bound is refused by the caller rather than silently
 * truncated here. */
#define PROMPT_CHOICE_MAX 64
/* The longest query prompt_read_choice() will accept.  Typing past it is
 * refused rather than truncated, and the refusal is retired as soon as
 * the query shrinks back under the cap -- BUF_NAME_QUERY_MAX's rule. */
#define PROMPT_CHOICE_QUERY_MAX 128

/* Read one of `choices` (at most PROMPT_CHOICE_MAX of them) in the echo
 * area.  Typing filters, Left/Right cycle the highlight, Tab completes
 * the typed text to the highlighted candidate, Enter answers.  `initial`
 * (NULL or "" for none) is the query the prompt starts with, as an
 * INITIAL-INPUT argument asks for.
 *
 * Enter on an empty query with nothing explicitly highlighted answers
 * with the EMPTY STRING -- the caller's default, whatever that is, is the
 * caller's to substitute; this is M-x's own rule for its "(default ...)"
 * and the only shape in which a Lisp DEFAULT argument can survive a
 * picker that always has a candidate highlighted.  Otherwise the
 * highlighted candidate wins; with no match at all, the typed text is
 * accepted when `require_match` is false and re-prompts when it is true.
 *
 * C-g and ESC cancel (MINIBUF_CANCELLED).  An answer too long for `out`
 * is MINIBUF_OVERFLOW and `out` is not written; nothing is ever delivered
 * truncated. */
enum minibuf_result prompt_read_choice(int fd, const char *prompt,
    const char *const *choices, int nchoices, const char *initial,
    bool require_match, char *out, int outsize);

/* editor_confirm_yn()'s tri-state core: the same one-key question, with
 * cancellation reported rather than folded into "no".  Every y/n question
 * in the editor goes through here, so they all agree on what a yes is and
 * on when the question is on screen. */
enum prompt_yn {
	PROMPT_YN_NO,
	PROMPT_YN_YES,
	PROMPT_YN_CANCELLED,
};

enum prompt_yn prompt_ask_yn(int fd, const char *question);

#endif /* KG_PROMPT_H */
