/*
 * kg's side of the regex differential test.
 *
 * Reads "<mode>\t<pattern>\t<subject>" lines from stdin and writes exactly
 * one result line per input line, in the protocol utils/regex_oracle.el
 * also speaks.  The modes are:
 *
 *	f	the first match from offset 0
 *	fa	every successive match, iterated the way a caller must
 *
 * and the answers are:
 *
 *	badpat			the pattern was rejected at compile time
 *	toocomplex		it compiled, but the step budget ran out
 *	nomatch			no match anywhere in the subject (mode f)
 *	match N s0 e0 s1 e1 ...	N spans as BYTE offsets, group 0 first,
 *				-1 -1 for a group that did not participate
 *	matches K <match>...	mode fa: K matches, each spelled as the
 *				"N s0 e0 ..." of a match line, so K == 0 is
 *				the mode's way of saying nomatch
 *
 * Byte offsets, not character offsets: Emacs reports character offsets and
 * the oracle converts.  utils/regex_differential.py drives both sides and
 * compares the two streams; `make check-regex-differential` runs it.
 *
 * Mode fa is where empty-match progress is decided: the scan position
 * after a match comes from kg_regex_next_offset(), never from "end + 1",
 * and both sides iterate that same rule.  MAX_REPORTED bounds the answer
 * so one pathological case cannot produce a megabyte line; both sides stop
 * at the same count.
 */
#include "regex.h"

#include <stdio.h>
#include <string.h>

/* Longer than any case the generator emits; over-long lines split and are
 * reported as bad input rather than silently mismatching the oracle. */
#define LINE_BYTES 4096
#define MAX_REPORTED 32

static void print_spans(const struct kg_match *m)
{
	int i;

	printf("%d", m->nspans);
	for (i = 0; i < m->nspans; i++) {
		printf(" %d %d", m->spans[i].start, m->spans[i].end);
	}
}

static void run_forward(const struct kg_regex *rx, const char *text)
{
	struct kg_match m;

	switch (kg_regex_match_forward(rx, text, 0, &m)) {
	case KG_REGEX_OK:
		printf("match ");
		print_spans(&m);
		printf("\n");
		return;
	case KG_REGEX_TOO_COMPLEX:
		printf("toocomplex\n");
		return;
	default:
		printf("nomatch\n");
		return;
	}
}

/* Every match, in order, iterated as src/search.c has to iterate them. */
static void run_forward_all(const struct kg_regex *rx, const char *text)
{
	struct kg_match found[MAX_REPORTED];
	int len = (int)strlen(text);
	int offset = 0;
	int count = 0;
	int i;

	while (count < MAX_REPORTED && offset >= 0 && offset <= len) {
		struct kg_match m;
		int status = kg_regex_match_forward(rx, text, offset, &m);

		if (status == KG_REGEX_TOO_COMPLEX) {
			printf("toocomplex\n");
			return;
		}
		if (status != KG_REGEX_OK) {
			break;
		}
		found[count++] = m;
		offset = kg_regex_next_offset(text, len, &m.spans[0]);
	}

	printf("matches %d", count);
	for (i = 0; i < count; i++) {
		printf(" ");
		print_spans(&found[i]);
	}
	printf("\n");
}

static void run_case(const char *mode, const char *pattern, const char *text)
{
	struct kg_regex rx;

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
	if (strcmp(mode, "f") == 0) {
		run_forward(&rx, text);
	} else if (strcmp(mode, "fa") == 0) {
		run_forward_all(&rx, text);
	} else {
		printf("badinput\n");
	}
}

int main(void)
{
	char line[LINE_BYTES];

	while (fgets(line, sizeof(line), stdin)) {
		char *nl = strchr(line, '\n');
		char *mode_end;
		char *tab;

		if (!nl) {
			printf("badinput\n");
			continue;
		}
		*nl = '\0';
		mode_end = strchr(line, '\t');
		if (!mode_end) {
			printf("badinput\n");
			continue;
		}
		*mode_end = '\0';
		tab = strchr(mode_end + 1, '\t');
		if (!tab) {
			printf("badinput\n");
			continue;
		}
		*tab = '\0';
		run_case(line, mode_end + 1, tab + 1);
	}
	return 0;
}
