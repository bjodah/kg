/* register.c — see register.h.
 *
 * Two halves, in this order: the table, which stores registers and knows
 * nothing about the terminal, and the commands, which read a name from
 * the keyboard and report what the table said. */

#include "register.h"

#include "bufhandle.h"
#include "def.h"
#include "edit.h"
#include "keyevent.h"
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

/* ---- The interactive half ----------------------------------------- */

/* Prompt with `prompt` and read one key as a register name.  Returns 0
 * once it has reported why the key was not one -- C-g, which is how any
 * prompt is escaped, or a key that cannot name a register -- so a caller
 * only has to stop.  A shutdown mid-prompt reports nothing and also
 * returns 0. */
static int register_read_name(int fd, const char *prompt)
{
	struct key_event c;

	editor_set_status_message("%s", prompt);
	editor_refresh_screen();
	c = editor_read_key(fd);
	if (!running) {
		return 0;
	}
	if (KEY_IS(c, 'g', KEY_MOD_CTRL)) {
		editor_set_status_message("Quit");
		return 0;
	}
	if (c.mods != 0 || !kg_register_name_valid(c.base)) {
		editor_set_status_message("Not a register name");
		return 0;
	}
	return (int)c.base;
}

/* Report `result` for register `name`, or the OK message when it is
 * KG_REGISTER_OK.  One place so every command spells a condition the
 * same way. */
static void register_report(
    int name, enum kg_register_result result, const char *ok)
{
	editor_set_status_message("Register %c: %s", name,
	    result == KG_REGISTER_OK ? ok : kg_register_result_message(result));
}

void editor_point_to_register(int fd)
{
	int name = register_read_name(fd, "Point to register: ");
	size_t pos;

	if (name == 0) {
		return;
	}
	pos = buffer_row_col_to_position(
	    bcur(), editor_current_filerow_or_eof(), editor_current_filecol());
	register_report(
	    name, kg_register_set_position(name, bcur(), pos), "point saved");
}

void editor_jump_to_register(int fd)
{
	int name = register_read_name(fd, "Jump to register: ");
	struct kg_buffer_handle handle;
	enum kg_register_result r;
	size_t pos;
	int row, col;

	if (name == 0) {
		return;
	}
	r = kg_register_get_position(name, &handle, &pos);
	if (r != KG_REGISTER_OK) {
		register_report(name, r, "");
		return;
	}
	/* The register names a buffer and a position, never a window: the
	 * selected window is what shows it, exactly as switching buffers
	 * by any other means would. */
	if (!buf_select(buf_handle_slot(handle))) {
		register_report(name, KG_REGISTER_STALE, "");
		return;
	}
	if (buffer_position_to_row_col(bcur(), pos, &row, &col) == 1) {
		editor_goto_line_direct(row + 1, col + 1);
	}
	register_report(name, KG_REGISTER_OK, "jumped");
}

void editor_copy_to_register(int fd)
{
	int name;
	char *text;
	int len;

	if (!kg_mark_is_set(bcur())) {
		editor_set_status_message("No mark set");
		return;
	}
	if (bcur()->rect_mode) {
		editor_set_status_message(
		    "copy-to-register: use C-x r k / C-x r y for rectangles");
		return;
	}
	name = register_read_name(fd, "Copy to register: ");
	if (name == 0) {
		return;
	}
	text = editor_get_region_text(&len);
	if (!text) {
		editor_set_status_message("Empty region");
		return;
	}
	register_report(name, kg_register_set_text(name, text, (size_t)len),
	    "region copied");
	free(text);
	bcur()->mark_highlight = 0;
	bcur()->shift_select = 0;
}

void editor_insert_register(int fd)
{
	const char *text;
	size_t len;
	enum kg_register_result r;
	int name;

	name = register_read_name(fd, "Insert register: ");
	if (name == 0) {
		return;
	}
	r = kg_register_get_text(name, &text, &len);
	if (r != KG_REGISTER_OK) {
		register_report(name, r, "");
		return;
	}
	/* Mark the start of the inserted text, as a yank does, so the span
	 * is a region again without having to remember where it began.
	 * KG_REGISTER_TEXT_MAX is far under INT_MAX, so the narrowing is
	 * exact for every entry this table can hold. */
	editor_push_mark();
	editor_insert_text_at_point(text, (int)len);
	register_report(name, KG_REGISTER_OK, "inserted");
}
