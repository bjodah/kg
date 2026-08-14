/* ======================= JSON for JSON-RPC ==============================
 *
 * See src/json.h for the contract.  This is Stage 2 of
 * doc/plans/2026-08-08-lsp.md and it links against the C library alone.
 *
 * The parser is recursive descent with one deliberate trick.  Children are
 * parsed onto a scratch stack that grows for the whole document, and a
 * container copies its own run off the top of that stack into the arena
 * when it closes -- so every array's elements and every object's members
 * end up in one contiguous block, indexing is a subscript rather than a
 * list walk, and the arena never has to be told a count in advance.  The
 * same stack serves every depth, because a container's children are always
 * the topmost run when it closes.
 *
 * Object members are stored as ordinary values carrying a key.  It costs
 * two words on every node and buys one node type, one scratch stack and
 * one copy path instead of two of each.
 */

#include "json.h"

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Arena blocks.  4 KiB is a page and holds a couple of hundred nodes; a
 * child array bigger than that gets a block of its own. */
#define KG_JSON_ARENA_BLOCK 4096u

/* The scratch value stack and the string scratch buffer both start here
 * and double.  Small enough that a two-field response allocates once. */
#define KG_JSON_SCRATCH_INIT 32u

struct arena_block {
	struct arena_block *next;
	size_t used;
	size_t cap;
	/* Declared as max_align_t so the bytes carved out of it are aligned
	 * for anything the arena is asked to hold. */
	max_align_t data[];
};

struct kg_json_value {
	enum kg_json_kind kind;
	/* The member name, when this node is an object member; NULL
	 * otherwise.  Not part of the value's identity -- no accessor reads
	 * it except kg_json_get() and kg_json_key_at(). */
	const char *key;
	size_t key_len;
	union {
		bool boolean;
		double number;
		struct {
			const char *ptr;
			size_t len;
		} string;
		struct {
			struct kg_json_value *items;
			size_t count;
		} list;
	} u;
};

struct kg_json {
	struct arena_block *blocks;
	struct kg_json_value root;
};

/* Parser state.  `err` is the offset the first refusal happened at, which
 * is the only thing a failed parse reports. */
struct parser {
	const char *text;
	size_t len;
	size_t pos;
	struct kg_json *doc;
	struct kg_json_value *stack;
	size_t stack_len;
	size_t stack_cap;
	char *sbuf;
	size_t sbuf_len;
	size_t sbuf_cap;
	unsigned depth;
	unsigned flags;
	size_t err;
};

/* ------------------------------- arena -------------------------------- */

static void *arena_alloc(struct kg_json *doc, size_t size)
{
	const size_t align = sizeof(max_align_t);
	struct arena_block *b;
	size_t cap;

	if (size > SIZE_MAX / 2) {
		return NULL;
	}
	size = (size + align - 1) / align * align;
	if (size == 0) {
		size = align;
	}
	b = doc->blocks;
	if (!b || b->cap - b->used < size) {
		cap = (size > KG_JSON_ARENA_BLOCK) ? size : KG_JSON_ARENA_BLOCK;
		b = malloc(sizeof(*b) + cap);
		if (!b) {
			return NULL;
		}
		b->next = doc->blocks;
		b->used = 0;
		b->cap = cap;
		doc->blocks = b;
	}
	b->used += size;
	return (char *)b->data + b->used - size;
}

static void arena_release(struct kg_json *doc)
{
	struct arena_block *b = doc->blocks;
	struct arena_block *next;

	while (b) {
		next = b->next;
		free(b);
		b = next;
	}
	doc->blocks = NULL;
}

/* ------------------------------ scratch ------------------------------- */

/* Both scratch buffers double, and both are freed when the parse ends
 * whatever its outcome.  Growth failure is reported as a parse failure at
 * the current offset; a document that cannot be held is not a document
 * whose error offset means anything, but the caller gets an error either
 * way and that is what matters. */
static bool grow(void **buf, size_t *cap, size_t need, size_t item)
{
	size_t next = *cap ? *cap : KG_JSON_SCRATCH_INIT;
	void *data;

	if (need <= *cap) {
		return true;
	}
	while (next < need) {
		if (next > SIZE_MAX / (2 * item)) {
			return false;
		}
		next *= 2;
	}
	data = realloc(*buf, next * item);
	if (!data) {
		return false;
	}
	*buf = data;
	*cap = next;
	return true;
}

static bool fail(struct parser *p)
{
	p->err = p->pos;
	return false;
}

static bool stack_push(struct parser *p, const struct kg_json_value *v)
{
	if (!grow((void **)&p->stack, &p->stack_cap, p->stack_len + 1,
		sizeof(*p->stack))) {
		return fail(p);
	}
	p->stack[p->stack_len++] = *v;
	return true;
}

static bool sbuf_push(struct parser *p, char c)
{
	if (!grow((void **)&p->sbuf, &p->sbuf_cap, p->sbuf_len + 1, 1)) {
		return fail(p);
	}
	p->sbuf[p->sbuf_len++] = c;
	return true;
}

/* ---------------------------- scanning ------------------------------- */

/* Reading past the end yields '\0', which is not a character any structural
 * position accepts, so every caller's own test rejects it and none of them
 * needs a length check first. */
static char peek(const struct parser *p)
{
	return (p->pos < p->len) ? p->text[p->pos] : '\0';
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static void skip_ws(struct parser *p)
{
	char c;

	while (p->pos < p->len) {
		c = p->text[p->pos];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
			return;
		}
		p->pos++;
	}
}

static size_t scan_digits(struct parser *p)
{
	size_t start = p->pos;

	while (is_digit(peek(p))) {
		p->pos++;
	}
	return p->pos - start;
}

/* ------------------------------ strings ------------------------------- */

static int hex_digit(char c)
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

static bool read_hex4(struct parser *p, unsigned *out)
{
	unsigned value = 0;
	int digit;
	int i;

	if (p->pos + 4 > p->len) {
		return fail(p);
	}
	for (i = 0; i < 4; i++) {
		digit = hex_digit(p->text[p->pos + i]);
		if (digit < 0) {
			return fail(p);
		}
		value = (value << 4) | (unsigned)digit;
	}
	p->pos += 4;
	*out = value;
	return true;
}

static bool sbuf_push_utf8(struct parser *p, unsigned cp)
{
	if (cp < 0x80u) {
		return sbuf_push(p, (char)cp);
	}
	if (cp < 0x800u) {
		return sbuf_push(p, (char)(0xC0u | (cp >> 6)))
		    && sbuf_push(p, (char)(0x80u | (cp & 0x3Fu)));
	}
	if (cp < 0x10000u) {
		return sbuf_push(p, (char)(0xE0u | (cp >> 12)))
		    && sbuf_push(p, (char)(0x80u | ((cp >> 6) & 0x3Fu)))
		    && sbuf_push(p, (char)(0x80u | (cp & 0x3Fu)));
	}
	return sbuf_push(p, (char)(0xF0u | (cp >> 18)))
	    && sbuf_push(p, (char)(0x80u | ((cp >> 12) & 0x3Fu)))
	    && sbuf_push(p, (char)(0x80u | ((cp >> 6) & 0x3Fu)))
	    && sbuf_push(p, (char)(0x80u | (cp & 0x3Fu)));
}

/* `\uXXXX`, with `p->pos` just past the `u`.  A high surrogate must be
 * followed by its low half spelled as a second escape -- that is the only
 * way UTF-16's astral characters can appear in JSON -- and a surrogate on
 * its own is refused rather than encoded, because the three-byte "UTF-8" of
 * a surrogate is not UTF-8 and passing it on would put an invalid sequence
 * into a buffer name or a file path. */
static bool parse_unicode_escape(struct parser *p)
{
	unsigned cp;
	unsigned low;

	if (!read_hex4(p, &cp)) {
		return false;
	}
	if (cp >= 0xDC00u && cp <= 0xDFFFu) {
		return fail(p);
	}
	if (cp >= 0xD800u && cp <= 0xDBFFu) {
		if (p->pos + 1 >= p->len || p->text[p->pos] != '\\'
		    || p->text[p->pos + 1] != 'u') {
			return fail(p);
		}
		p->pos += 2;
		if (!read_hex4(p, &low)) {
			return false;
		}
		if (low < 0xDC00u || low > 0xDFFFu) {
			return fail(p);
		}
		cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
	}
	return sbuf_push_utf8(p, cp);
}

static bool parse_escape(struct parser *p)
{
	char c;

	if (p->pos + 1 >= p->len) {
		return fail(p);
	}
	c = p->text[p->pos + 1];
	p->pos += 2;
	switch (c) {
	case '"':
		return sbuf_push(p, '"');
	case '\\':
		return sbuf_push(p, '\\');
	case '/':
		return sbuf_push(p, '/');
	case 'b':
		return sbuf_push(p, '\b');
	case 'f':
		return sbuf_push(p, '\f');
	case 'n':
		return sbuf_push(p, '\n');
	case 'r':
		return sbuf_push(p, '\r');
	case 't':
		return sbuf_push(p, '\t');
	case 'u':
		return parse_unicode_escape(p);
	default:
		p->pos -= 2;
		return fail(p);
	}
}

/* Copy the decoded scratch into the arena, NUL-terminated.  The terminator
 * is outside the reported length, so a string containing a NUL still says
 * how long it is. */
static bool arena_string(struct parser *p, const char **out, size_t *out_len)
{
	char *dst = arena_alloc(p->doc, p->sbuf_len + 1);

	if (!dst) {
		return fail(p);
	}
	if (p->sbuf_len > 0) {
		memcpy(dst, p->sbuf, p->sbuf_len);
	}
	dst[p->sbuf_len] = '\0';
	*out = dst;
	*out_len = p->sbuf_len;
	return true;
}

/* A string, with `p->pos` on its opening quote.  Bytes below 0x20 must be
 * escaped -- RFC 8259 says so, and a raw newline inside a string is the
 * signature of a server writing JSON with printf. */
static bool parse_string(struct parser *p, const char **out, size_t *out_len)
{
	unsigned char c;

	p->sbuf_len = 0;
	p->pos++;
	for (;;) {
		if (p->pos >= p->len) {
			return fail(p);
		}
		c = (unsigned char)p->text[p->pos];
		if (c == '"') {
			p->pos++;
			break;
		}
		if (c == '\\') {
			if (!parse_escape(p)) {
				return false;
			}
			continue;
		}
		if (c < 0x20) {
			return fail(p);
		}
		if (!sbuf_push(p, (char)c)) {
			return false;
		}
		p->pos++;
	}
	return arena_string(p, out, out_len);
}

/* ------------------------------ numbers ------------------------------- */

/* The integer part: an optional minus, then either a lone zero or a run of
 * digits that does not start with one.  The leading-zero rule is the whole
 * reason this is hand-written rather than left to strtod(): `01` is not
 * JSON, and strtod() reads it happily. */
static bool scan_int_part(struct parser *p)
{
	if (peek(p) == '-') {
		p->pos++;
	}
	if (peek(p) == '0') {
		p->pos++;
		return is_digit(peek(p)) ? fail(p) : true;
	}
	return (scan_digits(p) > 0) ? true : fail(p);
}

/* The optional fraction and exponent.  Both, when present, must have at
 * least one digit; `1.` and `1e` are not JSON. */
static bool scan_number_tail(struct parser *p)
{
	if (peek(p) == '.') {
		p->pos++;
		if (scan_digits(p) == 0) {
			return fail(p);
		}
	}
	if (peek(p) != 'e' && peek(p) != 'E') {
		return true;
	}
	p->pos++;
	if (peek(p) == '+' || peek(p) == '-') {
		p->pos++;
	}
	return (scan_digits(p) > 0) ? true : fail(p);
}

/* Validate the grammar, then let strtod() do the arithmetic on a copy.
 * kg never calls setlocale(), so the decimal point is '.' here as JSON
 * requires.  A magnitude a double cannot hold is refused rather than
 * becoming an infinity that would silently travel through the client as a
 * line number. */
static bool parse_number(struct parser *p, struct kg_json_value *out)
{
	char buf[KG_JSON_MAX_NUMBER_CHARS];
	size_t start = p->pos;
	size_t n;
	double value;

	if (!scan_int_part(p) || !scan_number_tail(p)) {
		return false;
	}
	n = p->pos - start;
	if (n >= sizeof(buf)) {
		p->pos = start;
		return fail(p);
	}
	memcpy(buf, p->text + start, n);
	buf[n] = '\0';
	value = strtod(buf, NULL);
	if (!(value >= -DBL_MAX && value <= DBL_MAX)) {
		p->pos = start;
		return fail(p);
	}
	out->kind = KG_JSON_NUMBER;
	out->u.number = value;
	return true;
}

/* ---------------------------- containers ------------------------------ */

static bool parse_value(struct parser *p, struct kg_json_value *out);

static bool enter(struct parser *p)
{
	if (p->depth >= KG_JSON_MAX_DEPTH) {
		return fail(p);
	}
	p->depth++;
	return true;
}

/* Move the run of children this container pushed (everything above `base`)
 * into one contiguous arena block and pop the scratch stack back. */
static bool take_children(
    struct parser *p, size_t base, struct kg_json_value **items, size_t *count)
{
	size_t n = p->stack_len - base;
	struct kg_json_value *dst = NULL;

	if (n > 0) {
		dst = arena_alloc(p->doc, n * sizeof(*dst));
		if (!dst) {
			return fail(p);
		}
		memcpy(dst, p->stack + base, n * sizeof(*dst));
	}
	p->stack_len = base;
	*items = dst;
	*count = n;
	return true;
}

/* One element or member, then the comma or the closing bracket after it.
 * Returns 1 to continue, 0 when the container just closed, -1 on error --
 * a trailing comma ends up at parse_value() on a `]`, which refuses it. */
static int parse_element(struct parser *p, char close, bool keyed)
{
	struct kg_json_value item = { 0 };

	if (keyed) {
		skip_ws(p);
		if (peek(p) != '"') {
			(void)fail(p);
			return -1;
		}
		if (!parse_string(p, &item.key, &item.key_len)) {
			return -1;
		}
		skip_ws(p);
		if (peek(p) != ':') {
			(void)fail(p);
			return -1;
		}
		p->pos++;
	}
	if (!parse_value(p, &item) || !stack_push(p, &item)) {
		return -1;
	}
	skip_ws(p);
	if (peek(p) == ',') {
		p->pos++;
		return 1;
	}
	if (peek(p) == close) {
		p->pos++;
		return 0;
	}
	(void)fail(p);
	return -1;
}

/* The members an object just closed are the topmost run of the scratch
 * stack, which is what makes this pairwise scan cheap enough to be honest:
 * it compares keys that are already decoded and already contiguous.  The
 * member bound is what keeps "pairwise" from meaning "quadratic in the file
 * size" (see KG_JSON_STRICT_MAX_MEMBERS).  The offset a refusal reports is
 * the closing brace, because a member does not remember where it was
 * written and a document that names one setting twice is wrong at the
 * object rather than at either spelling. */
static bool check_unique_keys(struct parser *p, size_t base, bool keyed)
{
	const struct kg_json_value *members = p->stack + base;
	size_t n = p->stack_len - base;
	size_t i;
	size_t j;

	if (!keyed || !(p->flags & KG_JSON_REJECT_DUPLICATE_KEYS)) {
		return true;
	}
	if (n > KG_JSON_STRICT_MAX_MEMBERS) {
		return fail(p);
	}
	for (i = 1; i < n; i++) {
		for (j = 0; j < i; j++) {
			if (members[i].key_len == members[j].key_len
			    && memcmp(members[i].key, members[j].key,
				   members[i].key_len)
				== 0) {
				return fail(p);
			}
		}
	}
	return true;
}

/* Arrays and objects differ in two characters and in whether an element
 * carries a name, so they are one function: the shape they share --
 * depth accounting, the empty case, the scratch run, the copy -- is all of
 * it. */
static bool parse_container(
    struct parser *p, struct kg_json_value *out, char close, bool keyed)
{
	size_t base = p->stack_len;
	int rc = 1;

	if (!enter(p)) {
		return false;
	}
	p->pos++;
	out->kind = keyed ? KG_JSON_OBJECT : KG_JSON_ARRAY;
	skip_ws(p);
	if (peek(p) == close) {
		p->pos++;
	} else {
		while (rc == 1) {
			rc = parse_element(p, close, keyed);
		}
		if (rc < 0) {
			return false;
		}
	}
	if (!check_unique_keys(p, base, keyed)) {
		return false;
	}
	p->depth--;
	return take_children(p, base, &out->u.list.items, &out->u.list.count);
}

static bool parse_literal(
    struct parser *p, const char *word, size_t n, struct kg_json_value *out)
{
	if (p->len - p->pos < n || memcmp(p->text + p->pos, word, n) != 0) {
		return fail(p);
	}
	p->pos += n;
	out->kind = (word[0] == 'n') ? KG_JSON_NULL : KG_JSON_BOOL;
	out->u.boolean = (word[0] == 't');
	return true;
}

static bool parse_value(struct parser *p, struct kg_json_value *out)
{
	skip_ws(p);
	switch (peek(p)) {
	case '{':
		return parse_container(p, out, '}', true);
	case '[':
		return parse_container(p, out, ']', false);
	case '"':
		out->kind = KG_JSON_STRING;
		return parse_string(p, &out->u.string.ptr, &out->u.string.len);
	case 't':
		return parse_literal(p, "true", 4, out);
	case 'f':
		return parse_literal(p, "false", 5, out);
	case 'n':
		return parse_literal(p, "null", 4, out);
	default:
		return parse_number(p, out);
	}
}

/* ---------------------------- public: read ---------------------------- */

struct kg_json *kg_json_parse(const char *text, size_t len, size_t *err_offset)
{
	return kg_json_parse_ex(text, len, 0u, err_offset);
}

struct kg_json *kg_json_parse_ex(
    const char *text, size_t len, unsigned flags, size_t *err_offset)
{
	struct parser p = { 0 };
	struct kg_json *doc;
	bool ok;

	if (err_offset) {
		*err_offset = 0;
	}
	if (!text || len > KG_JSON_MAX_INPUT_BYTES) {
		return NULL;
	}
	doc = calloc(1, sizeof(*doc));
	if (!doc) {
		return NULL;
	}
	p.text = text;
	p.len = len;
	p.doc = doc;
	p.flags = flags;
	ok = parse_value(&p, &doc->root);
	if (ok) {
		skip_ws(&p);
		/* Anything left over is a second document, or the tail of a
		 * message that was cut in half; either way this one is not
		 * what it claimed to be. */
		ok = (p.pos == p.len) || fail(&p);
	}
	free(p.stack);
	free(p.sbuf);
	if (ok) {
		return doc;
	}
	if (err_offset) {
		*err_offset = p.err;
	}
	kg_json_free(doc);
	return NULL;
}

void kg_json_free(struct kg_json *doc)
{
	if (!doc) {
		return;
	}
	arena_release(doc);
	free(doc);
}

const struct kg_json_value *kg_json_root(const struct kg_json *doc)
{
	return doc ? &doc->root : NULL;
}

enum kg_json_kind kg_json_kind_of(const struct kg_json_value *v)
{
	return v ? v->kind : KG_JSON_NONE;
}

const struct kg_json_value *kg_json_get(
    const struct kg_json_value *v, const char *key)
{
	const struct kg_json_value *m;
	size_t key_len;
	size_t i;

	if (!v || v->kind != KG_JSON_OBJECT || !key) {
		return NULL;
	}
	key_len = strlen(key);
	for (i = 0; i < v->u.list.count; i++) {
		m = &v->u.list.items[i];
		if (m->key_len == key_len
		    && memcmp(m->key, key, key_len) == 0) {
			return m;
		}
	}
	return NULL;
}

static bool is_list(const struct kg_json_value *v)
{
	return v && (v->kind == KG_JSON_ARRAY || v->kind == KG_JSON_OBJECT);
}

size_t kg_json_len(const struct kg_json_value *v)
{
	return is_list(v) ? v->u.list.count : 0;
}

const struct kg_json_value *kg_json_at(
    const struct kg_json_value *v, size_t index)
{
	if (!is_list(v) || index >= v->u.list.count) {
		return NULL;
	}
	return &v->u.list.items[index];
}

const char *kg_json_key_at(
    const struct kg_json_value *v, size_t index, size_t *len)
{
	const struct kg_json_value *m = kg_json_at(v, index);

	if (!m || !m->key) {
		return NULL;
	}
	if (len) {
		*len = m->key_len;
	}
	return m->key;
}

const char *kg_json_str(const struct kg_json_value *v, size_t *len)
{
	if (!v || v->kind != KG_JSON_STRING) {
		return NULL;
	}
	if (len) {
		*len = v->u.string.len;
	}
	return v->u.string.ptr;
}

double kg_json_num(const struct kg_json_value *v, double dflt)
{
	return (v && v->kind == KG_JSON_NUMBER) ? v->u.number : dflt;
}

long long kg_json_int(const struct kg_json_value *v, long long dflt)
{
	/* 2^63 exactly, and its negation: both are representable as
	 * doubles, and the half-open test they bracket is the one that
	 * admits exactly the values the cast below is defined for. */
	const double lo = -9223372036854775808.0;
	const double hi = 9223372036854775808.0;
	double value;

	if (!v || v->kind != KG_JSON_NUMBER) {
		return dflt;
	}
	value = v->u.number;
	if (!(value >= lo && value < hi)) {
		return dflt;
	}
	return (long long)value;
}

bool kg_json_bool(const struct kg_json_value *v, bool dflt)
{
	return (v && v->kind == KG_JSON_BOOL) ? v->u.boolean : dflt;
}

/* ---------------------------- public: write --------------------------- */

void kg_jsonw_init(struct kg_jsonw *w) { memset(w, 0, sizeof(*w)); }

void kg_jsonw_free(struct kg_jsonw *w)
{
	free(w->data);
	memset(w, 0, sizeof(*w));
}

/* The one allocation point.  Failure is recorded and never reported here:
 * every appender above it returns void, so a builder that ran out of
 * memory keeps taking calls and refuses only at kg_jsonw_finish().  One
 * spare byte is always kept so the terminator never needs a growth of its
 * own. */
static void w_append(struct kg_jsonw *w, const char *src, size_t len)
{
	if (w->failed) {
		return;
	}
	if (!grow((void **)&w->data, &w->cap, w->len + len + 1, 1)) {
		w->failed = true;
		return;
	}
	memcpy(w->data + w->len, src, len);
	w->len += len;
}

static void w_char(struct kg_jsonw *w, char c) { w_append(w, &c, 1); }

/* Separators, in the one place that knows about them: a comma before any
 * value that is not the first in its container, and nothing at all after a
 * key, which is why a key clears the flag its own separator set. */
static void w_before_value(struct kg_jsonw *w)
{
	if (w->pending_comma) {
		w_char(w, ',');
	}
	w->pending_comma = true;
}

static void w_escaped(struct kg_jsonw *w, const char *s, size_t len)
{
	static const char named[] = "\"\\\b\f\n\r\t";
	static const char *const escapes[]
	    = { "\\\"", "\\\\", "\\b", "\\f", "\\n", "\\r", "\\t" };
	char slot[8];
	const char *hit;
	size_t i;

	w_char(w, '"');
	for (i = 0; i < len; i++) {
		hit = memchr(named, s[i], sizeof(named) - 1);
		if (hit) {
			w_append(w, escapes[hit - named], 2);
		} else if ((unsigned char)s[i] < 0x20) {
			snprintf(slot, sizeof(slot), "\\u%04x",
			    (unsigned)(unsigned char)s[i]);
			w_append(w, slot, 6);
		} else {
			w_char(w, s[i]);
		}
	}
	w_char(w, '"');
}

void kg_jsonw_begin_object(struct kg_jsonw *w)
{
	w_before_value(w);
	w_char(w, '{');
	w->pending_comma = false;
	w->depth++;
}

void kg_jsonw_end_object(struct kg_jsonw *w)
{
	if (w->depth == 0) {
		w->failed = true;
		return;
	}
	w->depth--;
	w_char(w, '}');
	w->pending_comma = true;
}

void kg_jsonw_begin_array(struct kg_jsonw *w)
{
	w_before_value(w);
	w_char(w, '[');
	w->pending_comma = false;
	w->depth++;
}

void kg_jsonw_end_array(struct kg_jsonw *w)
{
	if (w->depth == 0) {
		w->failed = true;
		return;
	}
	w->depth--;
	w_char(w, ']');
	w->pending_comma = true;
}

void kg_jsonw_keyn(struct kg_jsonw *w, const char *key, size_t len)
{
	w_before_value(w);
	w_escaped(w, key, len);
	w_char(w, ':');
	w->pending_comma = false;
}

void kg_jsonw_key(struct kg_jsonw *w, const char *key)
{
	kg_jsonw_keyn(w, key, strlen(key));
}

void kg_jsonw_stringn(struct kg_jsonw *w, const char *s, size_t len)
{
	w_before_value(w);
	w_escaped(w, s, len);
}

void kg_jsonw_string(struct kg_jsonw *w, const char *s)
{
	kg_jsonw_stringn(w, s, strlen(s));
}

void kg_jsonw_int(struct kg_jsonw *w, long long value)
{
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%lld", value);

	w_before_value(w);
	if (n < 0 || (size_t)n >= sizeof(buf)) {
		w->failed = true;
		return;
	}
	w_append(w, buf, (size_t)n);
}

void kg_jsonw_bool(struct kg_jsonw *w, bool value)
{
	w_before_value(w);
	w_append(w, value ? "true" : "false", value ? 4 : 5);
}

void kg_jsonw_null(struct kg_jsonw *w)
{
	w_before_value(w);
	w_append(w, "null", 4);
}

/* The shortest decimal that reads back as `value`.  Seventeen significant
 * digits always round-trip a double and fifteen usually do, so the three
 * are tried in order and the first that survives strtod() is the one
 * written: it keeps a `0.1` a user typed from becoming
 * `0.10000000000000001` on its way to an adapter, without ever writing a
 * number that means something else.  kg never calls setlocale(), so the
 * decimal point is '.' as JSON requires. */
void kg_jsonw_number(struct kg_jsonw *w, double value)
{
	char buf[KG_JSON_MAX_NUMBER_CHARS];
	int precision;
	int n = 0;

	/* Neither an infinity nor a NaN can be spelled in JSON, and the words
	 * printf would write for them are not values any parser accepts. */
	if (!(value >= -DBL_MAX && value <= DBL_MAX)) {
		w->failed = true;
		return;
	}
	for (precision = 15; precision <= 17; precision++) {
		n = snprintf(buf, sizeof(buf), "%.*g", precision, value);
		if (n < 0 || (size_t)n >= sizeof(buf)) {
			w->failed = true;
			return;
		}
		if (strtod(buf, NULL) == value) {
			break;
		}
	}
	w_before_value(w);
	w_append(w, buf, (size_t)n);
}

/* One node, with its own depth guard.  The bound is the parser's, because
 * every node this is ever called on came from the parser and so is already
 * within it; the check is here anyway so that this function's recursion is
 * bounded by its own argument rather than by an invariant somewhere else. */
static void w_value(
    struct kg_jsonw *w, const struct kg_json_value *v, unsigned depth)
{
	size_t count = kg_json_len(v);
	const char *s;
	size_t len = 0;
	size_t i;

	if (depth > KG_JSON_MAX_DEPTH) {
		w->failed = true;
		return;
	}
	switch (kg_json_kind_of(v)) {
	case KG_JSON_NONE:
		w->failed = true;
		return;
	case KG_JSON_NULL:
		kg_jsonw_null(w);
		return;
	case KG_JSON_BOOL:
		kg_jsonw_bool(w, kg_json_bool(v, false));
		return;
	case KG_JSON_NUMBER:
		kg_jsonw_number(w, kg_json_num(v, 0.0));
		return;
	case KG_JSON_STRING:
		s = kg_json_str(v, &len);
		kg_jsonw_stringn(w, s, len);
		return;
	case KG_JSON_ARRAY:
		kg_jsonw_begin_array(w);
		for (i = 0; i < count; i++) {
			w_value(w, kg_json_at(v, i), depth + 1);
		}
		kg_jsonw_end_array(w);
		return;
	case KG_JSON_OBJECT:
		kg_jsonw_begin_object(w);
		for (i = 0; i < count; i++) {
			s = kg_json_key_at(v, i, &len);
			kg_jsonw_keyn(w, s ? s : "", s ? len : 0);
			w_value(w, kg_json_at(v, i), depth + 1);
		}
		kg_jsonw_end_object(w);
		return;
	}
	w->failed = true;
}

void kg_jsonw_value(struct kg_jsonw *w, const struct kg_json_value *v)
{
	w_value(w, v, 0);
}

void kg_jsonw_raw(struct kg_jsonw *w, const char *json, size_t len)
{
	w_before_value(w);
	w_append(w, json, len);
}

int kg_jsonw_finish(struct kg_jsonw *w, char **out, size_t *len)
{
	*out = NULL;
	*len = 0;
	if (w->failed || w->depth != 0 || !w->data) {
		kg_jsonw_free(w);
		return -1;
	}
	w->data[w->len] = '\0';
	*out = w->data;
	*len = w->len;
	memset(w, 0, sizeof(*w));
	return 0;
}
