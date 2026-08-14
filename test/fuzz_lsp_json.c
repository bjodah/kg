/* fuzz_lsp_json.c -- untrusted bytes through the JSON reader and writer.
 *
 * The bytes this parser eats in production come from a language server's
 * stdout, which is the least trusted input kg reads: any crash here is
 * remotely reachable by whatever is on PATH as clangd.  The whole input is
 * handed to kg_json_parse() as one document.
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

#include "../src/json.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void walk(const struct kg_json_value *v, int depth);

static void rewrite(struct kg_jsonw *w, const struct kg_json_value *v)
{
	size_t i, n, len;
	const char *s;

	switch (kg_json_kind_of(v)) {
	case KG_JSON_NULL:
		kg_jsonw_null(w);
		break;
	case KG_JSON_BOOL:
		kg_jsonw_bool(w, kg_json_bool(v, false));
		break;
	case KG_JSON_NUMBER: {
		/* kg_jsonw_int() is the only number appender; splice the
		 * double through raw so non-integers survive the trip.  A
		 * literal like 1e999 overflows strtod to infinity, which
		 * %g would spell as "inf" -- not JSON -- so respell it as
		 * an overflowing literal. */
		double x = kg_json_num(v, 0.0);
		char num[64];
		int k;

		if (isinf(x)) {
			k = snprintf(
			    num, sizeof(num), "%s1e999", x < 0 ? "-" : "");
		} else {
			k = snprintf(num, sizeof(num), "%.17g", x);
		}
		kg_jsonw_raw(w, num, (size_t)k);
		break;
	}
	case KG_JSON_STRING:
		s = kg_json_str(v, &len);
		kg_jsonw_stringn(w, s, len);
		break;
	case KG_JSON_ARRAY:
		kg_jsonw_begin_array(w);
		n = kg_json_len(v);
		for (i = 0; i < n; i++) {
			rewrite(w, kg_json_at(v, i));
		}
		kg_jsonw_end_array(w);
		break;
	case KG_JSON_OBJECT:
		kg_jsonw_begin_object(w);
		n = kg_json_len(v);
		for (i = 0; i < n; i++) {
			s = kg_json_key_at(v, i, &len);
			/* keys may hold NUL; the writer's key call is
			 * NUL-terminated, so splice the member only when
			 * the key survives that.  Skipped members drop
			 * out of the roundtrip comparison by design. */
			if (s && strlen(s) == len) {
				kg_jsonw_key(w, s);
				rewrite(w, kg_json_at(v, i));
			}
		}
		kg_jsonw_end_object(w);
		break;
	default:
		abort();
	}
}

static void compare(
    const struct kg_json_value *a, const struct kg_json_value *b)
{
	size_t i, n, alen, blen;
	const char *as, *bs;

	if (kg_json_kind_of(a) != kg_json_kind_of(b)) {
		abort();
	}
	switch (kg_json_kind_of(a)) {
	case KG_JSON_BOOL:
		if (kg_json_bool(a, false) != kg_json_bool(b, true)) {
			abort();
		}
		break;
	case KG_JSON_NUMBER: {
		double x = kg_json_num(a, 0.0), y = kg_json_num(b, 1.0);

		if (x != y && !(isnan(x) && isnan(y))) {
			abort();
		}
		break;
	}
	case KG_JSON_STRING:
		as = kg_json_str(a, &alen);
		bs = kg_json_str(b, &blen);
		if (alen != blen || memcmp(as, bs, alen) != 0) {
			abort();
		}
		break;
	case KG_JSON_ARRAY:
		n = kg_json_len(a);
		if (n != kg_json_len(b)) {
			abort();
		}
		for (i = 0; i < n; i++) {
			compare(kg_json_at(a, i), kg_json_at(b, i));
		}
		break;
	case KG_JSON_OBJECT: {
		/* Positional, not by name: a document may repeat a key,
		 * and both sides keep every member.  Members whose key
		 * holds a NUL were dropped by rewrite(); step past them
		 * on the original side. */
		size_t ai = 0, an = kg_json_len(a);

		n = kg_json_len(b);
		for (i = 0; i < n; i++) {
			while (ai < an) {
				as = kg_json_key_at(a, ai, &alen);
				if (as && strlen(as) == alen) {
					break;
				}
				ai++;
			}
			if (ai >= an) {
				abort();
			}
			as = kg_json_key_at(a, ai, &alen);
			bs = kg_json_key_at(b, i, &blen);
			if (alen != blen || memcmp(as, bs, alen) != 0) {
				abort();
			}
			compare(kg_json_at(a, ai), kg_json_at(b, i));
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
static void walk(const struct kg_json_value *v, int depth)
{
	size_t i, n, len;
	const char *s;

	if (depth > (int)KG_JSON_MAX_DEPTH + 2) {
		abort();
	}
	(void)kg_json_kind_of(v);
	(void)kg_json_get(v, "result");
	(void)kg_json_num(v, -1.0);
	(void)kg_json_int(v, -1);
	(void)kg_json_bool(v, false);
	s = kg_json_str(v, &len);
	if (s && s[len] != '\0') {
		abort();
	}
	n = kg_json_len(v);
	(void)kg_json_at(v, n);
	(void)kg_json_key_at(v, n, NULL);
	for (i = 0; i < n; i++) {
		(void)kg_json_key_at(v, i, &len);
		walk(kg_json_at(v, i), depth + 1);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct kg_json *doc, *redoc;
	struct kg_jsonw w;
	size_t err_offset = 0, out_len = 0;
	char *out = NULL;

	doc = kg_json_parse((const char *)data, size, &err_offset);
	if (!doc) {
		if (err_offset > size) {
			abort();
		}
		return 0;
	}
	walk(kg_json_root(doc), 0);
	kg_jsonw_init(&w);
	rewrite(&w, kg_json_root(doc));
	if (kg_jsonw_finish(&w, &out, &out_len) == 0) {
		redoc = kg_json_parse(out, out_len, NULL);
		if (!redoc) {
			/* What the builder emits must parse. */
			abort();
		}
		compare(kg_json_root(doc), kg_json_root(redoc));
		kg_json_free(redoc);
		free(out);
	}
	kg_jsonw_free(&w);
	kg_json_free(doc);
	return 0;
}
