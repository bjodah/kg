/* ========================= Keyboard macros =================================
 */

#include "def.h"
#include "kbd.h"
#include "keyevent.h"

#define MACRO_MAX 1024

/* One slot per key the reader saw, whichever of the three readers saw
 * it.  A byte editor_read_raw_byte() recorded (quoted-insert's literal
 * data, say) is a key_event with mods 0 and base equal to the byte --
 * the same encoding editor_read_key() uses for a bare, unmodified
 * character -- so the two interleave in one queue with no tag needed:
 * replay calls the readers in the same order recording did, and each
 * reader gets back exactly the shape it would have produced itself. */
static struct key_event macro_keys[MACRO_MAX];
static int macro_len = 0;
static int macro_pos = 0;
static int macro_recording = 0;
static int macro_replaying = 0;

int macro_is_recording(void) { return macro_recording; }

void macro_reset(void)
{
	macro_len = 0;
	macro_pos = 0;
	macro_recording = 0;
	macro_replaying = 0;
}

/* Called by the readers in tty.c: append key to buffer while recording.
 * Skipped during replay so we don't corrupt the buffer with replayed keys. */
void macro_on_key(struct key_event key)
{
	if (macro_recording && !macro_replaying && macro_len < MACRO_MAX) {
		macro_keys[macro_len++] = key;
	}
}

/* Called by the readers in tty.c: 1 with *out set to the next
 * pre-recorded key during replay, 0 when the buffer is exhausted (fall
 * back to the terminal). */
int macro_next_key(struct key_event *out)
{
	if (macro_replaying && macro_pos < macro_len) {
		*out = macro_keys[macro_pos++];
		return 1;
	}
	return 0;
}

/* C-x ( or F3: begin recording.  Resets any previously recorded macro. */
void macro_start(void)
{
	if (macro_replaying) {
		editor_set_status_message("Can't define macro while replaying");
		return;
	}
	macro_len = 0;
	macro_recording = 1;
	editor_set_status_message("Defining macro...");
}

/* C-x ) or F4 (while recording): stop recording.
 * trim: number of stop-key codes to discard from the end of the buffer
 * (the stop keys themselves were recorded before we knew to stop). */
void macro_stop(int trim)
{
	if (!macro_recording) {
		editor_set_status_message("Not defining a macro");
		return;
	}
	macro_recording = 0;
	macro_len -= trim;
	if (macro_len < 0) {
		macro_len = 0;
	}
	editor_set_status_message(
	    "Macro defined (%d key%s)", macro_len, macro_len == 1 ? "" : "s");
}

/* C-x e or F4 (when not recording): replay the last recorded macro.
 * count > 1 (from a C-u numeric argument) repeats the whole macro. */
void macro_replay(int fd, int count)
{
	if (macro_recording) {
		editor_set_status_message("Can't replay while defining macro");
		return;
	}
	if (macro_replaying) {
		return; /* no recursion */
	}
	if (macro_len == 0) {
		editor_set_status_message("No macro defined");
		return;
	}
	macro_replaying = 1;
	while (count-- > 0 && running) {
		macro_pos = 0;
		while (macro_pos < macro_len && running) {
			editor_process_keypress(fd);
		}
	}
	macro_replaying = 0;
	if (running) {
		editor_set_status_message("Macro replayed (%d key%s)",
		    macro_len, macro_len == 1 ? "" : "s");
	}
}
