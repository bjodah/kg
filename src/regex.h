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
#define KG_REGEX_TOODEEP 3

#define KG_REGEX_ICASE (1 << 0)

struct kg_span {
	int start;
	int end;
};

struct kg_match {
	int nspans;
	struct kg_span spans[RE_MAX_SPANS];
};

struct kg_regex {
	unsigned char storage[RE_MAX_COMPILED_BYTES];
	unsigned storage_size;
	re_t regex;
};

/* First glyph boundary at or after `requested`, clamped to [0, len].  A
 * stray continuation byte is a one-byte glyph, as in utf8_glyph_span_at().
 * Monotone, idempotent, and never below its argument. */
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

#endif /* KG_REGEX_H */
