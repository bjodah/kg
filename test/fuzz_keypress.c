#include "../src/def.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char *fuzz_input;
extern size_t fuzz_input_len;
extern size_t fuzz_input_pos;

static void free_rows(void)
{
	int i;

	for (i = 0; i < editor.numrows; i++) {
		editor_free_row(&editor.row[i]);
	}
	free(editor.row);
	editor.row = NULL;
	editor.numrows = 0;
}

static void reset_state(void)
{
	memset(&editor, 0, sizeof(editor));
	memset(buflist, 0, sizeof(buflist));
	memset(winlist, 0, sizeof(winlist));
	running = 1;
	suppress_undo = 0;
	global_auto_revert = 0;
	buf_current = 0;
	buf_count = 1;
	win_current = 0;
	win_count = 1;
	win_total_rows = 24;
	win_total_cols = 80;
	editor.screenrows = 24;
	editor.screencols = 80;
	editor.desired_visual_col = -1;
	editor.filename = strdup("fuzz.txt");
	winlist[0].active = 1;
	winlist[0].bufidx = 0;
	winlist[0].h = 24;
	winlist[0].w = 80;
	kill_ring_init();
	undo_init();
}

static void seed_buffer(const uint8_t *data, size_t size)
{
	size_t i;
	char line[64];
	int len = 0;

	for (i = 0; i < size; i++) {
		unsigned char ch = data[i];

		if ((ch & 0x0f) == 0) {
			editor_insert_row(editor.numrows, line, len);
			len = 0;
			continue;
		}
		if ((ch & 0x03) == 0) {
			ch = '\t';
		} else {
			ch = ' ' + (ch % 95);
		}
		if (len < (int)sizeof(line) - 1) {
			line[len++] = (char)ch;
		}
	}
	if (len > 0 || editor.numrows == 0) {
		editor_insert_row(editor.numrows, line, len);
	}
	editor.dirty = 0;
	undo_mark_clean();
}

static void teardown_state(void)
{
	undo_free();
	rect_kill_ring_free();
	kill_ring_free();
	free(editor.filename);
	editor.filename = NULL;
	free_rows();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	size_t seed_len;

	reset_state();
	seed_len = size > 32 ? size / 4 : size;
	seed_buffer(data, seed_len);

	fuzz_input = data + seed_len;
	fuzz_input_len = size - seed_len;
	fuzz_input_pos = 0;

	while (running && fuzz_input_pos < fuzz_input_len) {
		editor_process_keypress(-1);
	}

	teardown_state();
	return 0;
}
