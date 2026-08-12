#ifndef KG_JSON_H
#define KG_JSON_H

#include <stddef.h>

/* JSON, in the amount JSON-RPC needs and not a byte more.
 *
 * The Language Server Protocol's bodies are JSON, so the client needs to
 * read one and write one.  This module is that and nothing else: it knows
 * no editor state, no transport, no request semantics, and it depends on
 * the C library alone -- which is what lets a unit test link it by itself
 * and what keeps a third-party JSON library out of a repository whose rule
 * is no new dependencies.
 *
 * Two halves that barely meet.  Reading is a parser producing a read-only
 * tree; writing is an append-only builder producing bytes.  There is
 * deliberately no way to BUILD a tree and serialise it: kg's requests are
 * known at their call sites, so a builder is shorter to write, shorter to
 * read, and cannot allocate a node it forgets to free.
 *
 * The one crossing is kg_jsonw_value(), which writes a node that was
 * PARSED.  It exists for the debugger's launch configuration
 * (doc/plans/dap/01-protocol.md stage 4), whose `arguments` object is
 * validated, copied with substitutions applied inside its decoded strings,
 * and handed on as one value.  Doing that by splicing text into raw JSON
 * would be the bug: an environment variable holding `a"b\c` pasted
 * textually produces a document that is either invalid or attacker-shaped.
 *
 * Ownership is a single pointer on each side.  A parsed document owns
 * every node and every decoded string in one arena and is released by one
 * kg_json_free(); nothing in the tree is separately allocated, so there is
 * no partial free to get wrong and no node that outlives its document.  A
 * builder owns one growable buffer, released either to the caller by
 * kg_jsonw_finish() or back to the allocator by kg_jsonw_free().
 *
 * The parser is strict.  Trailing commas, comments, single quotes,
 * unquoted keys, `NaN` and `Infinity` -- every extension some JSON parser
 * accepts -- are syntax errors here, because a server that emits them is
 * broken in a way kg should report rather than guess at.  What it does
 * accept is anything RFC 8259 calls JSON, including an embedded NUL inside
 * a string, which is why the string accessor carries a length.
 */

/* Bounds, both of them the same kind of promise the transport's are: a
 * malformed or hostile document costs an error, not the address space.
 *
 * Nesting is bounded because the parser recurses, one C frame per
 * container: 64 is far past anything a language server sends (a
 * `textDocument/definition` reply nests five deep) and far short of what a
 * stack can take, so a document engineered as `[[[[[...` is refused
 * instead of exhausting the stack.
 *
 * The input bound matches LSP_TRANSPORT_MAX_BODY_BYTES rather than
 * including lsp_transport.h to say so: this module has no business knowing
 * what a transport is, and a parser called on a 32 MiB buffer from anywhere
 * else deserves the same answer.
 *
 * A number is bounded by the decimal text it is written as.  Doubles carry
 * 17 significant digits; anything spelling one in more than 63 characters
 * is padding, and refusing it means the conversion buffer is on the stack
 * and the parser allocates nothing per number. */
#define KG_JSON_MAX_DEPTH 64u
#define KG_JSON_MAX_INPUT_BYTES (32u * 1024u * 1024u)
#define KG_JSON_MAX_NUMBER_CHARS 64u

/* What a node is.  KG_JSON_NONE is not a JSON type: it is what
 * kg_json_kind_of() says about a NULL node, so that "the member is
 * absent" and "the member is null" stay two different answers.  JSON-RPC
 * needs exactly that distinction -- a response whose `result` is null
 * found nothing, while a response with no `result` at all is an error
 * reply -- and a parser that conflated them would make the client guess. */
enum kg_json_kind {
	KG_JSON_NONE = 0,
	KG_JSON_NULL,
	KG_JSON_BOOL,
	KG_JSON_NUMBER,
	KG_JSON_STRING,
	KG_JSON_ARRAY,
	KG_JSON_OBJECT,
};

/* The document (the arena and its root) and a node in it.  Both are opaque:
 * every question about a node is a function below, which is what lets the
 * representation change without a caller noticing, and what makes the
 * NULL-tolerance below a property of the module rather than a habit each
 * call site has to keep. */
struct kg_json;
struct kg_json_value;

/* Parse flags, for the one caller whose document is a FILE rather than a
 * message from a peer.
 *
 * KG_JSON_REJECT_DUPLICATE_KEYS turns `{"a":1,"a":2}` from a document whose
 * meaning depends on which member a reader happens to take into a syntax
 * error.  It is off by default because a protocol peer's duplicate member
 * is not worth failing a session over -- kg_json_get() takes the first, and
 * that is a defined answer -- and it is on for a configuration file, where
 * the two spellings of one setting are the user's mistake and reporting it
 * is the whole point.
 *
 * Asking for it also bounds how many members one object may have
 * (KG_JSON_STRICT_MAX_MEMBERS), because the check is pairwise: an object
 * with a hundred thousand members would otherwise cost a hundred million
 * comparisons to say something about.  A thousand members in one object is
 * already past anything a configuration file has a reason to hold. */
#define KG_JSON_REJECT_DUPLICATE_KEYS 1u
#define KG_JSON_STRICT_MAX_MEMBERS 1024u

/* Parse the whole of `text[0..len)` as one JSON document.  Returns NULL on
 * any syntax error, on exceeding a bound above, or on allocation failure;
 * `err_offset`, when given, is set to the byte offset the parser stopped
 * at, which is for tests and log lines rather than for recovery -- there is
 * no partial result and no resynchronisation.
 *
 * `text` need not be NUL-terminated and is not retained: every string in
 * the returned tree is a decoded copy in the document's own arena, so the
 * caller may reuse the buffer immediately.  That matters here, because the
 * buffer this is normally called on is the transport's, which the next
 * lsp_transport_next_message() invalidates.
 *
 * Leading and trailing whitespace is fine; anything else after the
 * document's single top-level value is an error, so a reply with a stray
 * half-message glued to it is refused rather than half-read. */
struct kg_json *kg_json_parse(const char *text, size_t len, size_t *err_offset);

/* The same parse under the flags above.  kg_json_parse() is this with no
 * flag set, which is what every protocol caller wants and why it stays the
 * short name. */
struct kg_json *kg_json_parse_ex(
    const char *text, size_t len, unsigned flags, size_t *err_offset);

/* Release a document and, with it, every node and string reachable from
 * it.  Any `struct kg_json_value *` obtained from it dangles afterwards.
 * NULL is accepted and ignored. */
void kg_json_free(struct kg_json *doc);

/* The document's single top-level value.  Never NULL for a document that
 * parsed. */
const struct kg_json_value *kg_json_root(const struct kg_json *doc);

/* Every accessor below takes NULL and answers it.  This is the whole point
 * of the reading half: a client asking for
 * `kg_json_get(kg_json_get(msg, "result"), "uri")` against a server that
 * sent neither member gets NULL, not a crash, and the check it writes is
 * the one at the end rather than one per step.  A wrong-kind node answers
 * the same way an absent one does, because a server that sends a number
 * where the protocol says string is not a case worth a separate branch. */

enum kg_json_kind kg_json_kind_of(const struct kg_json_value *v);

/* The member of an object named `key`, or NULL.  Keys are compared by
 * bytes, including any NUL inside them.  A document with the same key
 * twice keeps both members; this returns the first, which is the only
 * choice that does not depend on the parser's storage order. */
const struct kg_json_value *kg_json_get(
    const struct kg_json_value *v, const char *key);

/* Elements of an array, or members of an object in the order they were
 * written.  kg_json_len() is 0 for everything else, and kg_json_at() is
 * NULL out of range, so the usual `for (i = 0; i < len; i++)` needs no
 * guard of its own. */
size_t kg_json_len(const struct kg_json_value *v);
const struct kg_json_value *kg_json_at(
    const struct kg_json_value *v, size_t index);

/* The name of an object's `index`-th member, with its length through
 * `len`.  NULL for an array or out of range. */
const char *kg_json_key_at(
    const struct kg_json_value *v, size_t index, size_t *len);

/* A string node's bytes, or NULL for anything else.  The bytes are the
 * decoded ones -- escapes resolved, `\uXXXX` and surrogate pairs turned
 * into UTF-8 -- and they are BOTH NUL-terminated and length-carrying:
 * terminated because almost every caller wants to hand a `const char *` to
 * strcmp() or fopen(), and counted through `len` (which may be NULL)
 * because a JSON string may legally contain a NUL and the terminator then
 * lies about where it ends.
 *
 * Bytes above 0x7F are passed through as they arrived; this module does
 * not validate UTF-8.  Nothing here reaches a terminal -- display_glyph_at()
 * decides that, as it does for every other byte kg reads. */
const char *kg_json_str(const struct kg_json_value *v, size_t *len);

/* Numbers.  JSON has one numeric type and it is a double, so that is what
 * is stored and kg_json_num() is the lossless reading of it.
 *
 * kg_json_int() exists because the protocol's numbers are overwhelmingly
 * integers -- a line, a character, a request id -- and a caller should not
 * have to write the cast.  It truncates toward zero (2.9 is 2, -2.9 is -2),
 * so it is a conversion and not a rounding, and it returns `dflt` for a
 * node that is not a number at all and for a value outside long long's
 * range rather than invoking the undefined behaviour that cast would be. */
double kg_json_num(const struct kg_json_value *v, double dflt);
long long kg_json_int(const struct kg_json_value *v, long long dflt);

/* A bool node's value, or `dflt`.  JSON's true and false only: a number or
 * a string is not truthiness here, since the protocol never asks for it. */
bool kg_json_bool(const struct kg_json_value *v, bool dflt);

/* ------------------------------- writing ------------------------------ */

/* The builder.  Its fields are private; it is spelled out here only so a
 * caller can put one on the stack, which is where every one of them lives:
 * a request is built and sent inside one function.
 *
 * The reason it is a builder and not a tree is error handling.  Every
 * appender below returns void and checks one sticky failure flag, so a
 * fifteen-call request has one error check -- kg_jsonw_finish() -- instead
 * of fifteen, and a call site that forgets to check still cannot send half
 * a document: a builder that failed refuses to hand anything over.
 *
 * Commas and colons are the builder's business, not the caller's.  It
 * tracks whether the next value needs a comma before it, so call sites
 * read as the JSON they emit and no call site ever spells a separator. */
struct kg_jsonw {
	char *data;
	size_t len;
	size_t cap;
	unsigned depth;
	bool pending_comma;
	bool failed;
};

void kg_jsonw_init(struct kg_jsonw *w);

/* Discard a builder's buffer.  Needed only on the path that abandons a
 * half-built document (an error between begin and finish); after
 * kg_jsonw_finish() the builder holds nothing and calling this is
 * harmless. */
void kg_jsonw_free(struct kg_jsonw *w);

void kg_jsonw_begin_object(struct kg_jsonw *w);
void kg_jsonw_end_object(struct kg_jsonw *w);
void kg_jsonw_begin_array(struct kg_jsonw *w);
void kg_jsonw_end_array(struct kg_jsonw *w);

/* An object member's name, followed by exactly one value call.  The
 * builder does not check that -- it is not a validator -- but
 * kg_jsonw_finish() does refuse a document whose containers are not all
 * closed, which is the unbalanced-call mistake that actually happens. */
void kg_jsonw_key(struct kg_jsonw *w, const char *key);

/* The same, for a key that carries its length.  A JSON key may legally
 * contain a NUL, and a copied object's keys are copied rather than
 * shortened at the first one. */
void kg_jsonw_keyn(struct kg_jsonw *w, const char *key, size_t len);

/* Strings.  Escaping is not optional and not the caller's job: the quote,
 * the backslash and every byte below 0x20 are escaped on the way out, so a
 * file path with a backslash or a diagnostic with a newline in it cannot
 * end the string early.  Bytes above 0x7F are emitted as they are -- the
 * protocol's encoding is UTF-8 and re-encoding them as `\u` escapes would
 * be longer and no safer.
 *
 * kg_jsonw_string() takes a NUL-terminated C string; kg_jsonw_stringn()
 * takes bytes and a length, and is the one to use for buffer text, which
 * may contain anything. */
void kg_jsonw_string(struct kg_jsonw *w, const char *s);
void kg_jsonw_stringn(struct kg_jsonw *w, const char *s, size_t len);

void kg_jsonw_int(struct kg_jsonw *w, long long value);
void kg_jsonw_bool(struct kg_jsonw *w, bool value);
void kg_jsonw_null(struct kg_jsonw *w);

/* A number that is not an integer.  JSON has no way to spell an infinity or
 * a NaN, so one of those fails the builder rather than being written as a
 * word no parser accepts; every finite double is written as the SHORTEST
 * decimal that reads back as itself, so a `0.1` a user wrote stays `0.1`
 * and a value that survived a round trip is the same value. */
void kg_jsonw_number(struct kg_jsonw *w, double value);

/* Write a parsed node as one value, members and elements and all.  The
 * copy is exact in the ways that matter to a protocol peer: omission stays
 * omission (a member that was not there is not invented), null stays null
 * rather than becoming an omission, and a number stays a number rather
 * than being rounded into an integer -- debugpy errors on an explicit null
 * where it expects a member to be absent, so the difference is not
 * cosmetic.  What is not preserved is member ORDER's meaning, which JSON
 * gives none, and whitespace: this writes the canonical compact form.
 *
 * A NULL node writes nothing at all and fails the builder, since there is
 * no value to stand for it. */
void kg_jsonw_value(struct kg_jsonw *w, const struct kg_json_value *v);

/* Splice in bytes that are already JSON, as one value.  It is here for the
 * client layer's one recurring need -- a request id or a params blob kept
 * in its encoded form and copied into several messages -- and it is the one
 * call in this module that trusts its argument: nothing checks that the
 * bytes are valid JSON.  Do not reach for it with anything a server
 * sent. */
void kg_jsonw_raw(struct kg_jsonw *w, const char *json, size_t len);

/* Finish the document and hand its bytes over.  Returns 0 with `*out`
 * pointing at a malloc'd, NUL-terminated buffer the caller must free() and
 * `*len` its length (which is what to pass to lsp_transport_send(), since
 * the terminator is not part of the message); returns -1 if any appender
 * failed to allocate, if a container was left open, or if nothing was
 * written at all, having freed the buffer and set `*out` to NULL.  Either
 * way the builder is empty afterwards and needs no further release. */
int kg_jsonw_finish(struct kg_jsonw *w, char **out, size_t *len);

#endif /* KG_JSON_H */
