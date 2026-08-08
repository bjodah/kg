/* ============================== file: URIs ==============================
 *
 * Percent-encoding one way, percent-decoding the other, and a refusal for
 * everything that is not a local file.  See src/lsp_uri.h for the contract
 * and doc/plans/2026-08-08-lsp.md Stage 4 for why it is shared between the
 * client and the document sync.
 *
 * Both halves are byte loops with a bound check per byte, which is the
 * whole of the implementation: a URI parser that builds a structure would
 * be answering questions -- what the query string is, whether the host
 * resolves -- that no caller here has.
 */

#include "lsp_uri.h"

#include <string.h>

/* RFC 3986's unreserved set, plus the separator a path is made of.  A table
 * would be shorter to read and longer to check; this is the definition
 * spelled out, which is what a reader comparing it against the RFC wants. */
static bool uri_plain_byte(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
	    || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
	    || c == '~' || c == '/';
}

bool lsp_uri_from_path(const char *abs_path, char *out, size_t out_size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n;
	size_t i;

	if (!abs_path || abs_path[0] != '/' || out_size < 9) {
		return false;
	}
	memcpy(out, "file://", 7);
	n = 7;
	for (i = 0; abs_path[i]; i++) {
		unsigned char c = (unsigned char)abs_path[i];
		bool plain = uri_plain_byte(c);

		if (n + (plain ? 1u : 3u) >= out_size) {
			return false;
		}
		if (plain) {
			out[n++] = (char)c;
			continue;
		}
		out[n++] = '%';
		out[n++] = hex[c >> 4];
		out[n++] = hex[c & 0x0f];
	}
	out[n] = '\0';
	return true;
}

/* --------------------------------- reading ---------------------------- */

static char ascii_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Whether `s` begins with `prefix`, comparing ASCII letters without regard
 * to case.  Written out because the scheme and the host are the two parts
 * of a URI the RFC declares case-insensitive, and because strncasecmp() is
 * locale-dependent for exactly the bytes a path is most likely to hold. */
static bool ascii_prefix_ieq(const char *s, const char *prefix, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (ascii_lower(s[i]) != ascii_lower(prefix[i])) {
			return false;
		}
	}
	return true;
}

/* The value of one hexadecimal digit, or -1. */
static int hex_value(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/* Where the path starts in a `file:` URI, or NULL when the URI names
 * something kg cannot open.  The authority is whatever sits between the
 * `//` and the next `/`; empty and `localhost` both mean this host, and
 * anything else means another one. */
static const char *uri_path_start(const char *uri)
{
	static const char host[] = "localhost";
	const char *authority;
	const char *slash;
	size_t len;

	if (strlen(uri) < 8 || !ascii_prefix_ieq(uri, "file://", 7)) {
		return NULL;
	}
	authority = uri + 7;
	slash = strchr(authority, '/');
	if (!slash) {
		return NULL;
	}
	len = (size_t)(slash - authority);
	if (len == 0) {
		return slash;
	}
	if (len == sizeof(host) - 1 && ascii_prefix_ieq(authority, host, len)) {
		return slash;
	}
	return NULL;
}

bool lsp_uri_to_path(const char *uri, char *out, size_t out_size)
{
	const char *p = uri ? uri_path_start(uri) : NULL;
	size_t n = 0;

	if (!p || out_size < 2) {
		return false;
	}
	for (; *p; p++) {
		int hi, lo;

		if (n + 1 >= out_size) {
			return false;
		}
		if (*p != '%') {
			out[n++] = *p;
			continue;
		}
		hi = p[1] ? hex_value(p[1]) : -1;
		lo = (hi >= 0 && p[2]) ? hex_value(p[2]) : -1;
		if (lo < 0 || (hi == 0 && lo == 0)) {
			return false;
		}
		out[n++] = (char)((hi << 4) | lo);
		p += 2;
	}
	out[n] = '\0';
	return true;
}
