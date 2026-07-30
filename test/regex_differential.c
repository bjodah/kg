/*
 * kg's side of the regex differential test.
 *
 * Reads "<pattern>\t<subject>" lines from stdin and writes exactly one
 * result line per input line, in the protocol utils/regex_oracle.el also
 * speaks:
 *
 *	badpat			the pattern was rejected at compile time
 *	toocomplex		it compiled, but the step budget ran out
 *	nomatch			no match anywhere in the subject
 *	match N s0 e0 s1 e1 ...	N spans as BYTE offsets, group 0 first,
 *				-1 -1 for a group that did not participate
 *
 * Byte offsets, not character offsets: Emacs reports character offsets and
 * the oracle converts.  utils/regex_differential.py drives both sides and
 * compares the two streams; `make check-regex-differential` runs it.
 */
#include "regex.h"

#include <stdio.h>
#include <string.h>

/* Longer than any case the generator emits; over-long lines split and are
 * reported as bad input rather than silently mismatching the oracle. */
#define LINE_BYTES 4096

static void print_match(const struct kg_match *m)
{
	int i;

	printf("match %d", m->nspans);
	for (i = 0; i < m->nspans; i++) {
		printf(" %d %d", m->spans[i].start, m->spans[i].end);
	}
	printf("\n");
}

static void run_case(const char *pattern, const char *text)
{
	struct kg_regex rx;
	struct kg_match m;

	switch (kg_regex_compile(&rx, pattern, 0)) {
	case KG_REGEX_OK:
		break;
	case KG_REGEX_TOODEEP:
		printf("toocomplex\n");
		return;
	default:
		printf("badpat\n");
		return;
	}
	switch (kg_regex_match_forward(&rx, text, 0, &m)) {
	case KG_REGEX_OK:
		print_match(&m);
		return;
	case KG_REGEX_TOO_COMPLEX:
		printf("toocomplex\n");
		return;
	default:
		printf("nomatch\n");
		return;
	}
}

int main(void)
{
	char line[LINE_BYTES];

	while (fgets(line, sizeof(line), stdin)) {
		char *nl = strchr(line, '\n');
		char *tab;

		if (!nl) {
			printf("badinput\n");
			continue;
		}
		*nl = '\0';
		tab = strchr(line, '\t');
		if (!tab) {
			printf("badinput\n");
			continue;
		}
		*tab = '\0';
		run_case(line, tab + 1);
	}
	return 0;
}
