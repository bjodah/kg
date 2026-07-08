#ifndef KG_REGEX_H
#define KG_REGEX_H

#include "re.h"

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

int kg_regex_compile(struct kg_regex *rx, const char *pattern, int flags);

int kg_regex_match_forward(const struct kg_regex *rx, const char *text,
    int start_offset, struct kg_match *out);

int kg_regex_match_backward(const struct kg_regex *rx, const char *text,
    int before, struct kg_match *out);

#endif /* KG_REGEX_H */
