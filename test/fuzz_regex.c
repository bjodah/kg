/*
 * Fuzz harness for the tiny-regex-c engine via the kg_regex wrapper.
 *
 * Strategy: split the fuzz input into a pattern and a text, compile the
 * pattern, then match forward and backward against the text.  Regex
 * engines are historically rich bug-hunting ground.
 */
#include "../src/regex.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct kg_regex rx;
	struct kg_match match;
	char pattern[256];
	char text[512];
	size_t pat_len, txt_len;

	if (size < 2)
		return 0;

	/* First byte selects the pattern/text split point and flags. */
	int flags = (data[0] & 1) ? KG_REGEX_ICASE : 0;
	size_t split = 1 + (data[0] >> 1) % (size - 1);
	if (split > size)
		split = size;

	pat_len = split - 1;
	if (pat_len >= sizeof(pattern))
		pat_len = sizeof(pattern) - 1;
	memcpy(pattern, data + 1, pat_len);
	pattern[pat_len] = '\0';

	txt_len = size - split;
	if (txt_len >= sizeof(text))
		txt_len = sizeof(text) - 1;
	memcpy(text, data + split, txt_len);
	text[txt_len] = '\0';

	if (kg_regex_compile(&rx, pattern, flags) != KG_REGEX_OK)
		return 0;

	kg_regex_match_forward(&rx, text, 0, &match);
	kg_regex_match_backward(&rx, text, (int)txt_len, &match);

	return 0;
}
