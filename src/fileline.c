/* fileline.c — see fileline.h. */

#include "fileline.h"

#include "def.h"

#include <stdio.h>

/* How many bytes a single preview may walk past to reach its line.  The
 * read is line by line through a stdio buffer and holds nothing but the
 * preview itself, so the cost this bounds is time, not memory: a result on
 * line 900000 of a generated file must not make a listing pause.  One
 * mebibyte reaches past the end of every hand-written source file; beyond
 * it there is no preview, which is what the header promises. */
#define FILELINE_SCAN_MAX (1u << 20)

/* The blanks a preview drops from either end.  Not ascii_is_space(): a CR
 * or a form feed in the middle of a line is a control byte to neutralise
 * rather than an indentation to skip, and the two are different jobs. */
static bool fileline_is_blank(int c) { return c == ' ' || c == '\t'; }

/* Copy `text` into `out` for display: leading blanks skipped, the rest
 * truncated to fit.  Returns the bytes written, `out` NUL-terminated. */
static size_t preview_copy(
    const char *text, size_t len, char *out, size_t out_size)
{
	size_t i = 0;
	size_t n = 0;

	while (i < len && fileline_is_blank((unsigned char)text[i])) {
		i++;
	}
	while (i < len && n + 1 < out_size) {
		out[n++] = text[i++];
	}
	out[n] = '\0';
	return n;
}

/* Make `n` bytes of `out` safe and tidy to paint on one listing row: every
 * control byte becomes a space, then trailing blanks go.  Mapping rather
 * than dropping keeps the columns of what follows a stray control byte
 * where they were; the newline is the one that matters, because a listing
 * whose rows index its results cannot have a result spanning two of
 * them. */
static size_t preview_clean(char *out, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if ((unsigned char)out[i] < 0x20
		    || (unsigned char)out[i] == 0x7f) {
			out[i] = ' ';
		}
	}
	while (n > 0 && out[n - 1] == ' ') {
		n--;
	}
	out[n] = '\0';
	return n;
}

/* The buffer visiting `path`, or NULL.  Exactly buf_open_path()'s own rule
 * (buf_find_open() is its lookup), so the text a preview shows comes from
 * the buffer RET would land in and never from a second opinion about which
 * buffer that is. */
static const struct editor_buffer *fileline_open_buffer(const char *path)
{
	int slot = buf_find_open(path);

	return slot < 0 ? NULL : &buflist[slot];
}

/* Line `line` of `path` read from disk.  Bounded twice: FILELINE_SCAN_MAX
 * bytes walked in total, and the line itself copied only as far as `out`
 * reaches, with the remainder discarded rather than buffered.  Nothing is
 * read into memory that is not returned. */
static size_t line_from_disk(
    const char *path, int line, char *out, size_t out_size)
{
	FILE *fp = fopen(path, "rb");
	unsigned scanned = 0;
	bool leading = true;
	size_t n = 0;
	int at = 1;
	int c;

	if (!fp) {
		return 0;
	}
	while (
	    at < line && scanned < FILELINE_SCAN_MAX && (c = getc(fp)) != EOF) {
		scanned++;
		if (c == '\n') {
			at++;
		}
	}
	while (at == line && scanned < FILELINE_SCAN_MAX
	    && (c = getc(fp)) != EOF && c != '\n') {
		scanned++;
		leading = leading && fileline_is_blank(c);
		if (leading) {
			continue;
		}
		if (n + 1 >= out_size) {
			break; /* the rest of the line is past the cap */
		}
		out[n++] = (char)c;
	}
	fclose(fp);
	out[n] = '\0';
	return n;
}

size_t kg_file_line_preview(
    const char *path, int line, char *out, size_t out_size)
{
	const struct editor_buffer *b;

	if (!out || out_size == 0) {
		return 0;
	}
	out[0] = '\0';
	if (!path || !path[0] || line < 1) {
		return 0;
	}
	b = fileline_open_buffer(path);
	if (b) {
		/* An open buffer is the answer even when it has no such line:
		 * the file on disk is stale text, not a second chance. */
		if (line > b->numrows) {
			return 0;
		}
		return preview_clean(out,
		    preview_copy(b->row[line - 1].chars,
			(size_t)b->row[line - 1].size, out, out_size));
	}
	return preview_clean(out, line_from_disk(path, line, out, out_size));
}
