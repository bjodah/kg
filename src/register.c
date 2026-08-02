/* register.c — see register.h. */

#include "register.h"

#include "def.h"
#include "marker.h"

#include <stdlib.h>
#include <string.h>

/* One register.  `name` is meaningful only while `kind` is not
 * KG_REGISTER_EMPTY; a released slot is zeroed whole, so a stale name
 * never matches a lookup. */
struct kg_register_slot {
	unsigned char name;
	enum kg_register_kind kind;
	/* KG_REGISTER_POSITION */
	struct kg_buffer_handle buffer;
	struct kg_marker_handle marker;
	/* KG_REGISTER_TEXT: `len` is authoritative, `text` holds one
	 * defensive NUL past it. */
	char *text;
	size_t len;
};

static struct kg_register_slot registers[KG_REGISTER_SLOTS];
/* The sum of every live text register's `len`, kept in lock-step so a
 * store can check the table-wide cap without walking the table. */
static size_t register_text_total;

/* How many more text allocations this module may make before one of them
 * is failed on purpose.  Negative is how the editor always runs: no
 * seam.  See kg_register_fail_alloc_after() in register.h; mirrors
 * yank_alloc_budget in yank.c, one seam per allocator. */
static int register_alloc_budget = -1;

void kg_register_fail_alloc_after(int n) { register_alloc_budget = n; }

static bool register_alloc_ok(void)
{
	return register_alloc_budget < 0 || register_alloc_budget-- != 0;
}

bool kg_register_name_valid(int name) { return name > 0x20 && name < 0x7f; }

/* The slot holding `name`, or NULL. */
static struct kg_register_slot *register_find(int name)
{
	int i;

	if (!kg_register_name_valid(name)) {
		return NULL;
	}
	for (i = 0; i < KG_REGISTER_SLOTS; i++) {
		if (registers[i].kind != KG_REGISTER_EMPTY
		    && registers[i].name == (unsigned char)name) {
			return &registers[i];
		}
	}
	return NULL;
}

/* The first unused slot, or NULL when the table is full. */
static struct kg_register_slot *register_free_slot(void)
{
	int i;

	for (i = 0; i < KG_REGISTER_SLOTS; i++) {
		if (registers[i].kind == KG_REGISTER_EMPTY) {
			return &registers[i];
		}
	}
	return NULL;
}

/* Give up whatever this slot holds -- a marker or a text allocation --
 * and leave it empty.  Deleting a marker whose buffer is already gone is
 * harmless; kg_marker_delete() just says the handle resolves nothing. */
static void register_release(struct kg_register_slot *s)
{
	if (s->kind == KG_REGISTER_POSITION) {
		kg_marker_delete(s->marker);
	} else if (s->kind == KG_REGISTER_TEXT) {
		register_text_total -= s->len;
		free(s->text);
	}
	memset(s, 0, sizeof(*s));
}

enum kg_register_kind kg_register_kind_at(int name)
{
	const struct kg_register_slot *s = register_find(name);

	return s ? s->kind : KG_REGISTER_EMPTY;
}

enum kg_register_result kg_register_set_position(
    int name, struct editor_buffer *b, size_t pos)
{
	struct kg_register_slot *s;
	struct kg_marker_handle m;

	if (!kg_register_name_valid(name)) {
		return KG_REGISTER_BAD_NAME;
	}
	if (!b) {
		return KG_REGISTER_STALE;
	}
	s = register_find(name);
	if (!s && !(s = register_free_slot())) {
		return KG_REGISTER_FULL;
	}
	/* Create the new marker before releasing the old contents, so a
	 * failure leaves the register exactly as it was. */
	m = kg_marker_create(b, pos, KG_MARKER_GRAV_LEFT);
	if (m.id == 0) {
		return KG_REGISTER_NOMEM;
	}
	register_release(s);
	s->name = (unsigned char)name;
	s->kind = KG_REGISTER_POSITION;
	s->buffer = buf_handle_of(b);
	s->marker = m;
	return KG_REGISTER_OK;
}

enum kg_register_result kg_register_get_position(
    int name, struct kg_buffer_handle *out_buffer, size_t *out_pos)
{
	const struct kg_register_slot *s = register_find(name);
	size_t pos;

	if (!kg_register_name_valid(name)) {
		return KG_REGISTER_BAD_NAME;
	}
	if (!s) {
		return KG_REGISTER_UNSET;
	}
	if (s->kind != KG_REGISTER_POSITION) {
		return KG_REGISTER_WRONG_KIND;
	}
	if (!buf_resolve(s->buffer)
	    || kg_marker_resolve(s->marker, &pos) != KG_MARKER_OK) {
		return KG_REGISTER_STALE;
	}
	if (out_buffer) {
		*out_buffer = s->buffer;
	}
	if (out_pos) {
		*out_pos = pos;
	}
	return KG_REGISTER_OK;
}

enum kg_register_result kg_register_set_text(
    int name, const char *text, size_t len)
{
	struct kg_register_slot *s;
	size_t others;
	char *copy;

	if (!kg_register_name_valid(name)) {
		return KG_REGISTER_BAD_NAME;
	}
	if (len > KG_REGISTER_TEXT_MAX) {
		return KG_REGISTER_TOO_BIG;
	}
	s = register_find(name);
	if (!s && !(s = register_free_slot())) {
		return KG_REGISTER_FULL;
	}
	/* What this store replaces does not count against the total. */
	others
	    = register_text_total - (s->kind == KG_REGISTER_TEXT ? s->len : 0);
	if (len > KG_REGISTER_TEXT_TOTAL_MAX - others) {
		return KG_REGISTER_NO_ROOM;
	}
	if (!register_alloc_ok() || !(copy = malloc(len + 1))) {
		return KG_REGISTER_NOMEM;
	}
	memcpy(copy, text, len);
	copy[len] = '\0';
	register_release(s);
	s->name = (unsigned char)name;
	s->kind = KG_REGISTER_TEXT;
	s->text = copy;
	s->len = len;
	register_text_total += len;
	return KG_REGISTER_OK;
}

enum kg_register_result kg_register_get_text(
    int name, const char **out_text, size_t *out_len)
{
	const struct kg_register_slot *s = register_find(name);

	if (!kg_register_name_valid(name)) {
		return KG_REGISTER_BAD_NAME;
	}
	if (!s) {
		return KG_REGISTER_UNSET;
	}
	if (s->kind != KG_REGISTER_TEXT) {
		return KG_REGISTER_WRONG_KIND;
	}
	if (out_text) {
		*out_text = s->text;
	}
	if (out_len) {
		*out_len = s->len;
	}
	return KG_REGISTER_OK;
}

void kg_register_clear(void)
{
	int i;

	for (i = 0; i < KG_REGISTER_SLOTS; i++) {
		register_release(&registers[i]);
	}
	register_text_total = 0;
}

size_t kg_register_used(void)
{
	size_t used = 0;
	int i;

	for (i = 0; i < KG_REGISTER_SLOTS; i++) {
		if (registers[i].kind != KG_REGISTER_EMPTY) {
			used++;
		}
	}
	return used;
}

size_t kg_register_text_bytes(void) { return register_text_total; }

/* What each result says.  A switch rather than a table so that adding a
 * result without a message is a compiler diagnostic (-Wswitch) instead
 * of a hole discovered in the echo area. */
const char *kg_register_result_message(enum kg_register_result result)
{
	switch (result) {
	case KG_REGISTER_OK:
		return "";
	case KG_REGISTER_BAD_NAME:
		return "Not a register name";
	case KG_REGISTER_UNSET:
		return "Register is empty";
	case KG_REGISTER_WRONG_KIND:
		return "Register holds the other kind of thing";
	case KG_REGISTER_STALE:
		return "Register points into a buffer that is gone";
	case KG_REGISTER_FULL:
		return "All 32 registers are in use";
	case KG_REGISTER_TOO_BIG:
		return "Too much text for one register (1 MiB max)";
	case KG_REGISTER_NO_ROOM:
		return "Registers are full (4 MiB of text max)";
	case KG_REGISTER_NOMEM:
		return "Out of memory";
	}
	return "Register error";
}
