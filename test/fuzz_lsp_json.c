/* fuzz_lsp_json.c -- untrusted bytes through the LSP JSON reader and writer.
 *
 * The bytes this parser eats in production come from a language server's
 * stdout, which is the least trusted input kg reads: any crash here is
 * remotely reachable by whatever is on PATH as clangd.  The whole input is
 * handed to lsp_json_parse() as one document.
 *
 * On a parse failure the reported error offset must be inside the input.
 * On success the tree is walked exhaustively -- every accessor on every
 * node, including the wrong-kind and out-of-range calls the header
 * promises answer NULL -- and then re-serialised through the builder half
 * and parsed again.  The two trees must agree node for node: kind, length,
 * key and string bytes, number value.  A disagreement is one half of the
 * module lying about what the other wrote, which is exactly the bug class
 * a JSON-RPC client cannot tolerate.
 */

#include "../src/lsp_json.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void walk(const struct lsp_json_value *v, int depth);

static void rewrite(struct lsp_jsonw *w, const struct lsp_json_value *v)
{
	size_t i, n, len;
	const char *s;

	switch (lsp_json_kind_of(v)) {
	case LSP_JSON_NULL:
		lsp_jsonw_null(w);
		break;
	case LSP_JSON_BOOL:
		lsp_jsonw_bool(w, lsp_json_bool(v, false));
		break;
	case LSP_JSON_NUMBER: {
		/* lsp_jsonw_int() is the only number appender; splice the
		 * double through raw so non-integers survive the trip.  A
		 * literal like 1e999 overflows strtod to infinity, which
		 * %g would spell as "inf" -- not JSON -- so respell it as
		 * an overflowing literal. */
		double x = lsp_json_num(v, 0.0);
		char num[64];
		int k;

		if (isinf(x)) {
			k = snprintf(
			    num, sizeof(num), "%s1e999", x < 0 ? "-" : "");
		} else {
			k = snprintf(num, sizeof(num), "%.17g", x);
		}
		lsp_jsonw_raw(w, num, (size_t)k);
		break;
	}
	case LSP_JSON_STRING:
		s = lsp_json_str(v, &len);
		lsp_jsonw_stringn(w, s, len);
		break;
	case LSP_JSON_ARRAY:
		lsp_jsonw_begin_array(w);
		n = lsp_json_len(v);
		for (i = 0; i < n; i++) {
			rewrite(w, lsp_json_at(v, i));
		}
		lsp_jsonw_end_array(w);
		break;
	case LSP_JSON_OBJECT:
		lsp_jsonw_begin_object(w);
		n = lsp_json_len(v);
		for (i = 0; i < n; i++) {
			s = lsp_json_key_at(v, i, &len);
			/* keys may hold NUL; the writer's key call is
			 * NUL-terminated, so splice the member only when
			 * the key survives that.  Skipped members drop
			 * out of the roundtrip comparison by design. */
			if (s && strlen(s) == len) {
				lsp_jsonw_key(w, s);
				rewrite(w, lsp_json_at(v, i));
			}
		}
		lsp_jsonw_end_object(w);
		break;
	default:
		abort();
	}
}

static void compare(
    const struct lsp_json_value *a, const struct lsp_json_value *b)
{
	size_t i, n, alen, blen;
	const char *as, *bs;

	if (lsp_json_kind_of(a) != lsp_json_kind_of(b)) {
		abort();
	}
	switch (lsp_json_kind_of(a)) {
	case LSP_JSON_BOOL:
		if (lsp_json_bool(a, false) != lsp_json_bool(b, true)) {
			abort();
		}
		break;
	case LSP_JSON_NUMBER: {
		double x = lsp_json_num(a, 0.0), y = lsp_json_num(b, 1.0);

		if (x != y && !(isnan(x) && isnan(y))) {
			abort();
		}
		break;
	}
	case LSP_JSON_STRING:
		as = lsp_json_str(a, &alen);
		bs = lsp_json_str(b, &blen);
		if (alen != blen || memcmp(as, bs, alen) != 0) {
			abort();
		}
		break;
	case LSP_JSON_ARRAY:
		n = lsp_json_len(a);
		if (n != lsp_json_len(b)) {
			abort();
		}
		for (i = 0; i < n; i++) {
			compare(lsp_json_at(a, i), lsp_json_at(b, i));
		}
		break;
	case LSP_JSON_OBJECT: {
		/* Positional, not by name: a document may repeat a key,
		 * and both sides keep every member.  Members whose key
		 * holds a NUL were dropped by rewrite(); step past them
		 * on the original side. */
		size_t ai = 0, an = lsp_json_len(a);

		n = lsp_json_len(b);
		for (i = 0; i < n; i++) {
			while (ai < an) {
				as = lsp_json_key_at(a, ai, &alen);
				if (as && strlen(as) == alen) {
					break;
				}
				ai++;
			}
			if (ai >= an) {
				abort();
			}
			as = lsp_json_key_at(a, ai, &alen);
			bs = lsp_json_key_at(b, i, &blen);
			if (alen != blen || memcmp(as, bs, alen) != 0) {
				abort();
			}
			compare(lsp_json_at(a, ai), lsp_json_at(b, i));
			ai++;
		}
		break;
	}
	default:
		break;
	}
}

/* Every accessor on every node, wrong-kind and out-of-range included:
 * the header's promise is that each answers rather than crashes. */
static void walk(const struct lsp_json_value *v, int depth)
{
	size_t i, n, len;
	const char *s;

	if (depth > (int)LSP_JSON_MAX_DEPTH + 2) {
		abort();
	}
	(void)lsp_json_kind_of(v);
	(void)lsp_json_get(v, "result");
	(void)lsp_json_num(v, -1.0);
	(void)lsp_json_int(v, -1);
	(void)lsp_json_bool(v, false);
	s = lsp_json_str(v, &len);
	if (s && s[len] != '\0') {
		abort();
	}
	n = lsp_json_len(v);
	(void)lsp_json_at(v, n);
	(void)lsp_json_key_at(v, n, NULL);
	for (i = 0; i < n; i++) {
		(void)lsp_json_key_at(v, i, &len);
		walk(lsp_json_at(v, i), depth + 1);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct lsp_json *doc, *redoc;
	struct lsp_jsonw w;
	size_t err_offset = 0, out_len = 0;
	char *out = NULL;

	doc = lsp_json_parse((const char *)data, size, &err_offset);
	if (!doc) {
		if (err_offset > size) {
			abort();
		}
		return 0;
	}
	walk(lsp_json_root(doc), 0);
	lsp_jsonw_init(&w);
	rewrite(&w, lsp_json_root(doc));
	if (lsp_jsonw_finish(&w, &out, &out_len) == 0) {
		redoc = lsp_json_parse(out, out_len, NULL);
		if (!redoc) {
			/* What the builder emits must parse. */
			abort();
		}
		compare(lsp_json_root(doc), lsp_json_root(redoc));
		lsp_json_free(redoc);
		free(out);
	}
	lsp_jsonw_free(&w);
	lsp_json_free(doc);
	return 0;
}
