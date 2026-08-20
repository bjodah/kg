#ifndef KG_REGEX_H
#define KG_REGEX_H

#include "re.h"

/* kg's side of the regex engine.
 *
 * Every offset and span here is a BYTE offset into `text`, which is
 * NUL-terminated: the engine takes no length, so a subject with an
 * embedded NUL ends there and is unsupported until an explicit-length API
 * lands.
 *
 * A start/before offset handed in must sit on a kg glyph boundary.  One
 * that does not is normalized FORWARD by kg_utf8_forward_boundary(), and
 * no match is ever reported starting before that normalized request.
 *
 * Empty matches are ordinary results, so a caller walking every match must
 * take its next scan position from kg_regex_next_offset() -- adding a byte
 * lands inside a glyph and, on an empty match, never terminates.
 *
 * The failure statuses mean different things and none of them means "no
 * match": KG_REGEX_BADPAT is a pattern that does not compile, TOODEEP a
 * compile that ran out of budget, TOO_COMPLEX a match attempt that did.
 * They are #defines rather than an enum because callers compare a plain
 * `int status` against them. */
#define KG_REGEX_OK 0
#define KG_REGEX_NOMATCH 1
#define KG_REGEX_BADPAT 2
#define KG_REGEX_TOODEEP 3 /* compile ran out of budget */
#define KG_REGEX_TOO_COMPLEX 4 /* a match attempt ran out of budget */

#define KG_REGEX_ICASE (1 << 0)

struct kg_span {
	int start;
	int end;
};

struct kg_match {
	int nspans;
	struct kg_span spans[RE_MAX_SPANS];
};

/* `regex` points into `storage`, so a kg_regex is not copyable: copying
 * one leaves the copy's `regex` aimed at the original's bytes.  The
 * alignment is the engine's published requirement for caller storage --
 * it reads the compiled program's multi-byte fields in place, and refuses
 * a buffer that is not aligned for them. */
struct kg_regex {
	alignas(
	    RE_STORAGE_ALIGNMENT) unsigned char storage[RE_MAX_COMPILED_BYTES];
	unsigned storage_size;
	re_t regex;
};

/* First glyph boundary at or after `requested`, clamped to [0, len].  A
 * stray continuation byte is a one-byte glyph, as in utf8_glyph_span_at().
 * Monotone, idempotent, and never below its argument.  `text` holds at
 * least `len` bytes; `requested` may be anything. */
int kg_utf8_forward_boundary(const char *text, int len, int requested);

/* Offset at which to resume scanning after `match`: its end when it
 * consumed anything, the next glyph boundary when it was empty, and -1
 * when the subject is exhausted (an empty match at `len`).  Never returns
 * a value <= match->start. */
int kg_regex_next_offset(
    const char *text, int len, const struct kg_span *match);

int kg_regex_compile(struct kg_regex *rx, const char *pattern, int flags);

int kg_regex_match_forward(const struct kg_regex *rx, const char *text,
    int start_offset, struct kg_match *out);

int kg_regex_match_backward(const struct kg_regex *rx, const char *text,
    int before, struct kg_match *out);

/* ---- the bounded window -------------------------------------------
 *
 * The two calls above search the whole of `text` from an offset; the two
 * below search a WINDOW of it, [start_offset, limit] in bytes of `text`
 * -- the same coordinate space as the offsets and spans everywhere else
 * in this header.  Forward answers the FIRST match in the window,
 * backward the LAST; that is the only difference between them.
 *
 * `limit` is where CONSUMPTION stops, not where the subject stops.  No
 * reported span ends past it, and no repetition, alternative or group may
 * consume a character that would cross it -- enforced DURING matching, so
 * backtracking sees it: a greedy branch hands characters back at the
 * limit, an alternative that cannot fit under it loses to a later one AT
 * THE SAME START, and the scan then goes on to starts the limit still
 * allows.  Filtering an unbounded answer instead reaches none of those
 * three, which is what made a bounded search miss matches Emacs finds.
 *
 * THE ANCHORS ARE INDEPENDENT OF THE LIMIT.  `\'` and `$` keep asking
 * about `text`'s own terminator and `\`` and `^` about `text` itself, so
 * a row-end-anchored pattern does not match at the limit and a
 * row-start-anchored one does not match at `start_offset`.  Handing the
 * engine a truncated copy of the row would answer differently, which is
 * why this is a parameter and not a shorter subject.
 *
 * A match may end exactly AT `limit`; one needing one more byte may not.
 * An empty match at `limit` is an ordinary result, so a pattern that can
 * match empty still matches at `start_offset` whenever
 * `start_offset <= limit`.  A multi-byte character starting under the
 * limit and ending past it is not consumed at all, which is also what a
 * `limit` landing inside a glyph means: that whole glyph is out of the
 * window.  Unlike `start_offset`, the limit is therefore not normalized
 * to a boundary -- consumption is by whole character, so it needs no
 * help.
 *
 * KG_REGEX_LIMIT_NONE, and any limit at or past `strlen(text)`, are the
 * unbounded call: byte for byte what the two calls above answer.  A
 * `start_offset` past `limit`, and a negative limit that is not
 * KG_REGEX_LIMIT_NONE, are KG_REGEX_NOMATCH.
 *
 * The backward call sweeps candidate starts forward and keeps the last
 * match ending at or before `limit`, HANDING THE SAME LIMIT TO EVERY
 * CANDIDATE.  That is what makes it agree with a bounded forward search
 * rather than with a filter over unbounded ones: a start whose preferred
 * branch crosses the limit yields its shorter branch instead of dropping
 * out of the sweep.  An empty match exactly at `limit` is not a match
 * BEFORE it, so backward does not report one -- the one asymmetry
 * between the two directions, and the rule the unbounded
 * kg_regex_match_backward() has always had. */
#define KG_REGEX_LIMIT_NONE (-1)

int kg_regex_match_forward_bounded(const struct kg_regex *rx, const char *text,
    int start_offset, int limit, struct kg_match *out);

int kg_regex_match_backward_bounded(const struct kg_regex *rx,
    const char *text, int start_offset, int limit, struct kg_match *out);

#endif /* KG_REGEX_H */
