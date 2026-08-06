/* ==================== Describing commands and keys ====================== */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "describe.h"

#include "bufmgr.h"
#include "cmd.h"
#include "def.h"
#include "keyevent.h"
#include "keymap.h"

/* The page being built, and whether it stopped fitting.
 *
 * One bounded buffer rather than rows appended as they are formatted:
 * buf_append_special_text() splits text into rows itself, so this file
 * never touches a row primitive, and the whole page lands in the buffer
 * in one call or not at all.  describe-bindings over every built-in map
 * is the largest thing that goes through here. */
static char page[16384];
static size_t page_used;
static int page_full;

static void page_reset(void)
{
	page_used = 0;
	page_full = 0;
	page[0] = '\0';
}

/* Append one line.  Once the page is full it stays full: a later line
 * that happened to fit would leave a hole in the middle of the output
 * with nothing saying so. */
[[gnu::format(printf, 1, 2)]] static void page_line(const char *fmt, ...)
{
	size_t room = sizeof(page) - page_used;
	va_list ap;
	int written;

	if (page_full) {
		return;
	}
	va_start(ap, fmt);
	/* C23 expands va_start to __builtin_c23_va_start, which clang's
	 * valist checker does not model, so it reports `ap` as
	 * uninitialized here.  Same suppression as the other va_start
	 * sites in the editor.  `room` is hoisted out so the call fits on
	 * the one line the suppression covers. */
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	written = vsnprintf(page + page_used, room, fmt, ap);
	va_end(ap);
	if (written < 0 || page_used + (size_t)written >= sizeof(page)) {
		page[page_used] = '\0';
		page_full = 1;
		return;
	}
	page_used += (size_t)written;
}

/* Show the page as a read-only buffer in this window.
 *
 * Wrapping is the display's: the buffer is opened in visual-line mode, so
 * a long summary folds at the window edge instead of running off it, and
 * this file has no width model of its own to keep in step with the
 * renderer's. */
static void page_show(const char *what)
{
	int slot
	    = buf_prepare_special_text(DESCRIBE_BUFFER_NAME, &text_syntax, 1);

	if (slot < 0 || buf_append_special_text(slot, page, page_used)) {
		editor_set_status_message("%s: no room for the answer", what);
		return;
	}
	buflist[slot].visual_line_mode = 1;
	buf_select(slot);
	wcur()->cx = wcur()->cy = wcur()->rowoff = wcur()->coloff = 0;
	editor_set_status_message(
	    "%s%s — q closes", what, page_full ? " (truncated)" : "");
}

/* The `index`th binding with its sequence already spelled out -- the
 * shape both walks below want.  Returns 0 when there is no such binding,
 * or when its keys have no spelling. */
static int describe_binding_text(
    int index, struct keymap_binding *binding, char *text, size_t size)
{
	return !keymap_binding_at(index, binding)
	    && !keymap_format_sequence(
		binding->keys, binding->count, text, size);
}

/* Every sequence bound to `name` right now, comma-separated.  Returns
 * how many there were; a sequence that will not fit is left out rather
 * than cut in half, since half a spelling names a different key. */
static int describe_bound_keys(const char *name, char *out, size_t size)
{
	int i, found = 0;
	size_t used = 0;

	out[0] = '\0';
	for (i = 0; i < keymap_binding_count(); i++) {
		struct keymap_binding binding;
		char text[KEYMAP_SEQUENCE_FORMAT_MAX];

		if (!describe_binding_text(i, &binding, text, sizeof(text))
		    || !binding.command || strcmp(binding.command, name)
		    || used + strlen(text) + 3 > size) {
			continue;
		}
		used += (size_t)snprintf(
		    out + used, size - used, "%s%s", found++ ? ", " : "", text);
	}
	return found;
}

/* What the registry knows about `name`: what it does, what it is allowed
 * to do, and every key that runs it.  A name with no descriptor is not an
 * error here -- a keymap leaf may name a runtime command that has been
 * removed, and saying so is the answer. */
static void describe_the_command(const char *name)
{
	const struct named_cmd *cmd = cmd_lookup(name);
	char keys[KEYMAP_SEQUENCE_FORMAT_MAX * 6];

	page_line("%s\n", name);
	page_line("  %s\n",
	    cmd ? cmd->summary
		: cmd_id_by_name(name)
		? "Defined in Lisp"
		: "No command of that name is defined now");
	if (cmd) {
		page_line("  %s in a read-only buffer\n",
		    (cmd->flags & CMD_EDITS_BUFFER) ? "Refused" : "Allowed");
		page_line("  %s\n",
		    (cmd->flags & CMD_LISP_CALLABLE)
			? "Callable from Lisp with (command-execute ...)"
			: "Not callable from Lisp");
	}
	page_line("  %s\n",
	    describe_bound_keys(name, keys, sizeof(keys)) ? keys
							  : "Not on any key");
}

/* Read a key sequence the way dispatch reads one: keys until the maps
 * stop saying "a longer sequence starts with this".  Returns how many
 * keys, or 0 when the editor is going away.  `match` is what the last
 * lookup said, so the caller does not have to ask twice. */
static int describe_read_sequence(
    int fd, struct key_event *keys, struct keymap_match *match)
{
	char sofar[KEYMAP_SEQUENCE_FORMAT_MAX] = "";
	int count = 0;

	while (count < KEYMAP_SEQUENCE_MAX) {
		editor_set_status_message("Describe key: %s", sofar);
		editor_refresh_screen();
		keys[count] = editor_read_key(fd);
		if (!running) {
			return 0;
		}
		count++;
		keymap_lookup(keys, count, match);
		if (match->result != KEYMAP_PREFIX) {
			break;
		}
		(void)keymap_format_sequence(keys, count, sofar, sizeof(sofar));
	}
	return count;
}

/* What a lookup that did not land on a command has to say for itself.
 * Every enumerator keymap_lookup() can report has an entry, so the table
 * is total over its argument and needs no range check of its own. */
static const char *describe_verdict(enum keymap_result result)
{
	static const char *const verdicts[] = {
		[KEYMAP_NO_MATCH] = "Not bound",
		[KEYMAP_PREFIX] = "A prefix: it waits for the next key",
		[KEYMAP_COMMAND] = "",
		[KEYMAP_UNRESOLVED] = "",
		[KEYMAP_AMBIGUOUS]
		= "Bound two ways at once, which is a configuration error",
	};

	return verdicts[result];
}

void describe_key(int fd)
{
	struct key_event keys[KEYMAP_SEQUENCE_MAX];
	struct keymap_match match, under;
	char text[KEYMAP_SEQUENCE_FORMAT_MAX];
	int count = describe_read_sequence(fd, keys, &match);

	if (!count || keymap_format_sequence(keys, count, text, sizeof(text))) {
		editor_set_status_message("");
		return;
	}
	page_reset();
	page_line("%s\n\n", text);
	/* `command` is set for a resolved binding and for one whose command
	 * is gone; both have something to describe.  Everything else has
	 * only a verdict. */
	if (!match.command) {
		page_line("  %s\n", describe_verdict(match.result));
		page_show("Describe key");
		return;
	}
	page_line("  Bound in the %s map\n\n", keymap_name(match.map));
	describe_the_command(match.command);
	/* Truthful rather than inferred: the same sequence is looked up
	 * again with the winning layer switched off, so this reports the
	 * binding the user would get, not the one precedence suggests. */
	keymap_lookup_shadowed(keys, count, match.map, &under);
	if (under.command) {
		page_line("\n  Shadows %s, from the %s map\n", under.command,
		    keymap_name(under.map));
	}
	page_show("Describe key");
}

/* Prompt for a command name.  Returns 0 when the user cancelled or
 * entered nothing.
 *
 * The prompt prefills from `name`, so it is emptied here rather than at
 * each call site: reaching editor_read_line() with an uninitialised
 * buffer is a read of whatever the stack held. */
static int describe_read_name(int fd, const char *prompt, char *name, int size)
{
	name[0] = '\0';
	return editor_read_line(fd, prompt, name, size) == MINIBUF_ACCEPTED
	    && name[0];
}

void describe_command(int fd)
{
	char name[64];

	if (!describe_read_name(fd, "Describe command: ", name, sizeof(name))) {
		return;
	}
	page_reset();
	describe_the_command(name);
	page_show("Describe command");
}

void describe_bindings(int fd)
{
	int i;

	(void)fd;
	page_reset();
	/* Enumeration order is the order the maps were installed in, not
	 * precedence, and saying otherwise here would be wrong: the global
	 * map is built first and is the layer that loses.  describe-key is
	 * where precedence is answered, for one key, truthfully. */
	page_line("Key bindings, and the map each is in.\n");
	page_line("describe-key says which one wins here.\n\n");
	for (i = 0; i < keymap_binding_count(); i++) {
		struct keymap_binding binding;
		char text[KEYMAP_SEQUENCE_FORMAT_MAX];

		if (!describe_binding_text(i, &binding, text, sizeof(text))) {
			continue;
		}
		/* Key column padded, the rest not: a padded command column
		 * would push the map name past a narrow window for every
		 * row, and the display would fold all of them. */
		page_line("%-11s %s (%s)\n", text,
		    binding.command ? binding.command : "a prefix",
		    keymap_name(binding.map));
	}
	page_show("Describe bindings");
}

/* where-is answers in the echo area, the way Emacs' does: the answer is
 * one line, and opening a buffer to hold it would lose the place the
 * user was asking from. */
void describe_where_is(int fd)
{
	char name[64];
	char keys[KEYMAP_SEQUENCE_FORMAT_MAX * 6];

	if (!describe_read_name(fd, "Where is command: ", name, sizeof(name))) {
		return;
	}
	if (!cmd_id_by_name(name)) {
		editor_set_status_message("%s is not a command", name);
		return;
	}
	if (!describe_bound_keys(name, keys, sizeof(keys))) {
		editor_set_status_message("%s is not on any key", name);
		return;
	}
	editor_set_status_message("%s is on %s", name, keys);
}
