/* fuzz_width.c -- arbitrary bytes through the one glyph seam.
 *
 * Every byte kg reads is eventually drawn or measured through
 * display_glyph_at() (src/width.c), which is also the boundary where a
 * byte that must never reach the terminal as itself gets its visible
 * spelling.  The input is used directly as a buffer and every byte
 * offset is asked about, which is exactly the renderer's and the column
 * arithmetic's access pattern -- including offsets that land inside a
 * UTF-8 sequence, which is what horizontal scroll does.
 *
 * Oracles, beyond ASan catching any out-of-bounds read:
 *  - a glyph's span stays inside the buffer and within UTF-8's 1..4;
 *  - an escape spelling fits the esc[] field and its width is its length;
 *  - a passthrough glyph's bytes are the source bytes at their offset;
 *  - the per-byte measurement loop (utf8_width_at summed over every
 *    byte) and the per-glyph walk (stepping by span) agree on the total
 *    width, which is the agreement between how a run is measured and how
 *    it is drawn;
 *  - utf8_codepoint_at()'s span agrees with the glyph's.
 */

#include "../src/def.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define FUZZ_WIDTH_MAX_LEN (1 << 16)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const char *buf = (const char *)data;
	int len = size > FUZZ_WIDTH_MAX_LEN ? FUZZ_WIDTH_MAX_LEN : (int)size;
	int i, span;
	long byte_total = 0, glyph_total = 0;

	for (i = 0; i < len; i++) {
		struct display_glyph g;

		display_glyph_at(buf, len, i, &g);
		if (g.span < 1 || g.span > 4 || i + g.span > len) {
			abort();
		}
		if (g.width < 0 || g.width > 4 || g.len < 0) {
			abort();
		}
		if (g.bytes == g.esc) {
			/* An escape spelling ("^X", "\xnn") or a covered
			 * continuation byte drawing nothing. */
			if (g.len > KG_DISPLAY_ESCAPE_MAX - 1
			    || (g.len > 0 && g.width != g.len)) {
				abort();
			}
		} else {
			if (g.bytes != buf + i || g.len != g.span
			    || g.width > 2) {
				abort();
			}
		}
		utf8_codepoint_at(buf, len, i, &span);
		if (g.len > 0 && span != g.span) {
			abort();
		}
		byte_total += utf8_width_at(buf, len, i);
	}
	for (i = 0; i < len;) {
		struct display_glyph g;

		display_glyph_at(buf, len, i, &g);
		glyph_total += g.width;
		i += g.span;
	}
	if (byte_total != glyph_total) {
		abort();
	}
	if (utf8_display_width(buf, len) != byte_total) {
		abort();
	}
	return 0;
}
