# Plan 02 — Editing, undo, row, and prefix invariants

## Goal

Fix the small editor-state defects that can corrupt text or misreport state,
and add a reference-model test layer before the mutation architecture expands.

This plan covers UTF-8 Backspace and forward-delete corruption; the wrong undo
payload for `C-k` at end-of-line; stale clean checkpoints after undo-history
eviction; row slices copied without a guaranteed NUL terminator; the explicit
prefix zero lost across `C-x`; and stateful UTF-8 edit/undo testing.

Do not wait for the future edit-transaction project (plan 10). These fixes are
independent and should land first. Line numbers below were re-verified against
the working tree; re-read the function before editing.

## Read first

- `src/buffer.c`: `editor_insert_row()` (325), `editor_row_append_string()`
  (866), `editor_row_del_char()` (892), `editor_point_row()` (1155),
  `editor_del_char()` (1167), `editor_del_forward_char()` (1213),
  `editor_self_insert_glyph()` (1336, the model to copy), `editor_kill_line()`
  (1366).
- `src/yank.c`: `editor_delete_text_range_raw()` (422) — it lives here, not in
  `buffer.c`; `sort_lines_cmp()` (583).
- `src/undo.c` (339 lines, read it all): `undo_push()` (39) including the
  eviction block (83-108), `editor_undo()` (113), `undo_mark_clean()` (339).
- `src/def.h`: `struct undo_op` (356), `struct undo_stack` (369),
  `MAX_UNDO_SIZE` (366), `struct command_prefix` (253), the prefix fields of
  `struct editor_config` (276-287), `utf8_glyph_span_at()` (691) and
  `utf8_glyph_start_before()` (720).
- `src/kbd.c`: `handle_universal_arg()` (114), `handle_cx_prefix_key()` (368),
  the prefix-dispatch block (537-578), `case CTRL_X:` (758-761).
- `test/test_buffer.c`, `test/test_undo.c`, `test/fuzz_keypress.c`.
- PTY precedents: `test/pty/self-insert-utf8-undo.yaml`,
  `test/pty/macro-prefix-key-reapat-n-times.yaml`,
  `test/pty/mx-repeat-prefix-passthrough.yaml`.

## Required invariants

1. Cursor, mark, and edit boundaries never bisect a structurally valid UTF-8
   glyph; one user deletion removes and restores one whole glyph.
2. Undo payload bytes exactly equal the bytes removed by the forward operation.
3. Every `erow` owns `chars[size] == '\0'`.
4. A buffer is clean only when its current history state equals the last
   successful save state.
5. Prefix presence is independent of numeric prefix value; zero is valid.

## Phase 1 — Delete whole glyphs, with a matching undo payload

### Verified defect

`editor_del_char()` (buffer.c:1194-1203) records
`undo_push(UNDO_DELETE_CHAR, filerow, filecol - 1, row->chars[filecol - 1], NULL, 0)`
and calls `editor_row_del_char(row, filecol - 1)`, which memmoves exactly one
byte away (buffer.c:892-901). `editor_del_forward_char()` (1236-1240) does the
same at `filecol`. So Backspace on `é` leaves the lone `0xA9` continuation byte
in the row, and undo re-inserts one byte through
`editor_row_insert_char(row, op->col, op->c)` (undo.c:146) — where `op->c` came
from a plain `char` and is therefore *negative* on a signed-char target before
being truncated back on assignment. `editor.cx` also moves by one byte, so
point can land mid-glyph.

The correct pattern already exists in the same file:
`editor_self_insert_glyph()` (buffer.c:1336-1358) pushes `UNDO_REPLACE_TEXT`
with the full old span and calls `editor_delete_text_range_raw()`. Copy it.

### Files

`src/buffer.c`, `src/def.h` (only if the helper needs external linkage),
`test/test_buffer.c`, `test/test_undo.c`, `test/pty/`.

### Changes

Add a private helper in `src/buffer.c`:

```c
/* Delete `len` bytes at (row, col) as one undoable step.  Returns 0 and
 * leaves the buffer untouched when the undo payload cannot be recorded. */
static int editor_delete_span_with_undo(int row, int col, int len);
```

Behavior: validate row/column/length without mutating; record the undo payload
**first** (bail out on failure, as `editor_self_insert_glyph()` does with
`editor_nomem()`); delete exactly `len` bytes via
`editor_delete_text_range_raw()` (yank.c:422, declared def.h:969), which already
suppresses nested undo and calls `editor_update_row()`; update point once.

Reuse `UNDO_REPLACE_TEXT` with `op->c == 0` (replacement length zero) — its
reverse in undo.c:236-247 already skips the delete when `op->c <= 0` and then
re-inserts `op->text`, which is exactly delete-undo. Do **not** extend
`UNDO_DELETE_CHAR.c` to encode partial multibyte state, and do not add a new
opcode unless the reviewer asks for the clearer name.

`editor_del_char()`: keep `UNDO_JOIN_LINE` for the `filecol == 0` join
(buffer.c:1182-1193). Otherwise take
`glyph_start = utf8_glyph_start_before(row->chars, row->size, filecol)`, delete
`filecol - glyph_start` bytes, and move point to `glyph_start` — note the
existing code decrements `editor.coloff` or `editor.cx` by one (1199-1203); the
replacement must decrement by the glyph's byte length, honouring the same
`coloff` case, or use `editor_cursor_goto(filerow, glyph_start)`.

`editor_del_forward_char()`: keep the next-line join at EOL (1226-1235).
Otherwise take `utf8_glyph_span_at(row->chars, row->size, filecol)` and delete
that whole span without moving point.

Malformed-byte policy: a stray byte is a one-byte glyph. Both helpers already
return 1 in that case (def.h:700-713, 731-734), so nothing extra is needed —
just assert it in the tests.

### Tests

`test_buffer` and `test_undo` both link `yank.o`, so
`editor_delete_text_range_raw()` is available (see `EXTRA_buffer`/`EXTRA_undo`
in the Makefile). Follow the `setup()`/`teardown()` shape at the top of
`test/test_undo.c`.

Native table over: ASCII; 2-byte `é` (`\xC3\xA9`); 3-byte CJK; 4-byte emoji;
a combining mark as its own codepoint; a double-width glyph; a stray
continuation byte; a truncated lead; BOL/EOL and row joins. For each, assert the
exact remaining bytes, `editor.cx`/`editor.coloff`, `editor_visual_col()`, the
`chars[size] == '\0'` invariant, that **one** undo restores the exact bytes and
position, and that `editor.dirty` moves as expected. Add an allocation-failure
case if a malloc hook is available; otherwise assert the early-return path by
inspection and note it.

PTY: extend the pattern of `test/pty/self-insert-utf8-undo.yaml`. Backspace
(`C-?`) and forward-delete (`C-d`) over 2-, 3-, and 4-byte glyphs, each followed
by `C-_` (undo), with `expected_saved` spelling the exact bytes. Keep
`key_delay` at the default — under 0.03 s kg treats the keys as a paste
(`editor.paste_mode`, kbd.c:533) and skips auto-indent/autocompletion.

## Phase 2 — Correct `kill-line` newline undo

### Verified defect

`editor_kill_line()` (buffer.c:1379-1392), in the `filecol == row->size` branch,
appends `"\n"` to the kill ring but pushes
`undo_push(UNDO_KILL_TEXT, filerow, filecol, 0, editor.row[filerow + 1].chars, editor.row[filerow + 1].size)`
— the *next row's contents*, not the newline. `UNDO_KILL_TEXT`'s reverse
(undo.c:219-225) re-inserts `op->text` at the point, so undoing `C-k` on
`a\nb` yields `abb` rather than `a\nb`. The comment above the function
(buffer.c:1360-1365) claims kill-ring and undo stay in lock-step; today they do
not for this branch.

### Files

`src/buffer.c`, `test/test_undo.c`, `test/pty/kill-line-eol-undo.yaml` (new).

### Changes

In that branch: keep `kill_ring_append("\n", 1)`; push
`undo_push(UNDO_KILL_TEXT, filerow, filecol, 0, "\n", 1)`; then join as before.
The join leaves the next row's bytes in the buffer, so re-inserting a `"\n"` at
`(filerow, filecol)` splits them back apart — that is `editor_insert_text_raw()`'s
existing multi-row behaviour, so no new code is needed.

`C-u N C-k` batches N calls in `kbd.c`; each pushes its own record. Characterise
that granularity in a test before changing it. The first patch repairs the
payload only.

### Tests

`test/test_undo.c` already has `test_kill_line`; add siblings for: `a\nb` →
`C-k` at EOL → `ab` → undo → `a\nb`; next row empty; next row starting with a
multibyte glyph; two consecutive EOL kills then two undos; and the kill ring
holding exactly `"\n"` (not the next row) after the EOL kill. Add the PTY case
with `expected_saved` to prove the whole path including cursor placement.

## Phase 3 — Give clean checkpoints stable identity

### Verified defect

`undo_mark_clean()` (undo.c:339) stores `clean_size = undostack.size`, a plain
count. `undo_push()` evicts from the **tail** when `size > max_size`
(undo.c:83-108) without touching `clean_size`, and `editor_undo()` sets
`editor.dirty = 0` whenever `undostack.size == undostack.clean_size`
(undo.c:325-327). Once the record that separated the clean state from the
current one has been evicted, the count matches a different state.

Note also that `undo_free()` (undo.c:22-36) resets `size` but not `clean_size`,
and `editor.dirty` is a monotonically increasing counter that undo never
decrements — the count comparison is the *only* way a buffer becomes clean
again.

### Files

`src/undo.c`, `src/def.h`, `test/test_undo.c`; save/clean callers in
`src/fileio.c` (428, 504) and `src/bufmgr.c` (238, 1253, 1546).

### Minimal fix

`clean_size = C` means "the saved state is reached by popping down to C
records", so it is only meaningful while the bottom-most C records are still on
the stack. Eviction always removes from the **tail**, i.e. from the bottom, and
it only triggers on a push (so at least one record was added after the save).
Any eviction therefore invalidates the checkpoint unconditionally:

```c
/* inside the trim block in undo_push(), after prev->next = NULL */
undostack.clean_size = -1;
```

Also reset `clean_size = -1` in `undo_free()` (undo.c:22), which currently
clears `size` but leaves the checkpoint pointing at a stack that no longer
exists. Write the failing test first.

### Preferred follow-up

Replace count-only cleanliness with a monotonically increasing state identity:

```c
struct undo_stack {
	...
	uint64_t current_state;
	uint64_t clean_state;
};
```

Rules: every committed user-visible edit advances `current_state`; undo moves to
the predecessor identity stored on the record; save sets
`clean_state = current_state`; evicting history never makes two unrelated states
compare equal; a new edit after undo creates a fresh identity; and wraparound is
handled deliberately (practically unreachable, but compare safely). This needs
`before_state`/`after_state` on `struct undo_op`, or one grouped undo node. Do
not infer clean state from stack length once branching histories are possible.

Both `struct undo_stack` instances matter: the global `undostack` (undo.c:10)
and the per-buffer copy in `struct editor_buffer` (def.h:408). Whatever fields
you add must survive `buf_temp_swap_in()`/`buf_temp_swap_out()`.

### Tests

`undostack` is a plain global, so a test can set `undostack.max_size = 2` after
`undo_init()`. Reproducer: edit A; `undo_mark_clean()`; edit B; edit C (evicts
A); undo C; assert `editor.dirty != 0`.

Also cover: save at an empty stack; save after several edits; undo back to clean
and away again; a new edit after undo; per-buffer stacks across `C-x b`; and a
compound operation that emits several internal records (`M-q` reflow,
`editor_sort_lines()`). Reference property: `editor.dirty == 0` iff the flattened
buffer bytes equal the last saved snapshot.

## Phase 4 — Make row constructors own termination

### Verified defect

`editor_insert_row()` (buffer.c:325-339) does
`newchars = malloc(len + 1); memcpy(newchars, s, len + 1);` — it copies the byte
*after* the caller's slice and never writes a terminator of its own. Callers
that pass an interior slice therefore produce a row whose `chars[size]` is not
`'\0'`. Confirmed offenders:

- `src/undo.c:275` (`UNDO_RECT_OVERWRITE` replay) and `:308`/`:312`
  (`UNDO_REFLOW_PARA` replay) — slices bounded by `'\n'`, so `chars[size]`
  becomes `'\n'`;
- `src/bufmgr.c:1994`/`:1998` (`buf_replace_special_text()`) — the `nl - p`
  case copies `'\n'`; the `end - p` case reads `text[text_length]`, one past the
  caller's buffer, which is a genuine out-of-bounds read when the caller did not
  NUL-terminate;
- `src/dired.c:234` (`dired_add_row()`) when `len < strlen(line)`.

This is not purely cosmetic: `sort_lines_cmp()` (yank.c:583) compares rows with
`strcmp(ra->chars, rb->chars)`, which walks past the `malloc(len + 1)`
allocation on such a row. ASan will flag it.

### Files

`src/buffer.c`, `src/undo.c`, `src/bufmgr.c`, `src/dired.c`,
`test/test_buffer.c`, `test/test_undo.c`.

### Changes

Change `editor_insert_row(int at, const char *s, size_t len)` to: checked-add
`len + 1` (use the existing `checked_add_size_t()`/`checked_add_int_size()`
helpers in `def.h` that `editor_row_append_string()` already uses);
`memcpy(newchars, s, len)`; `newchars[len] = '\0'`. Never read `s[len]`.

Then audit the other `(pointer, length)` APIs for the same hidden-terminator
assumption:

```sh
rg -n 'memcpy\([^;]*len\s*\+\s*1|strcpy\(|strcmp\(|strlen\(' src
```

Classify each as an explicit byte slice (the callee terminates if it stores a C
string) or a NUL string (the length must be derived or validated consistently).
`editor_row_append_string()` (buffer.c:884-886) is already correct and is the
model.

Add debug-only row assertions after insert/delete/undo/reflow/rectangle paths —
a `static void assert_row_terminated(const erow *r)` compiled under `NDEBUG`
control, called from `editor_update_row()`.

### Tests

- call `editor_insert_row()` with an interior slice followed by `'\n'` and
  assert `row->chars[row->size] == '\0'`;
- place the slice at the very end of a heap allocation so ASan catches the
  one-past read (the current code fails this today);
- `M-q` reflow, undo, then `editor_sort_lines()` over the reconstructed rows —
  this is the end-to-end reproducer for the `strcmp` overrun;
- rectangle kill/yank, undo, then assert every row terminator;
- note that the existing tests pass string literals
  (`editor_insert_row(0, "hllo", 4)`), where `s[len]` happens to be `'\0'` —
  that is why the bug has stayed invisible.

## Phase 5 — Preserve prefix presence across sub-prefixes

### Verified defect

`struct command_prefix { int supplied; int value; }` already exists (def.h:253)
and `editor.current_prefix` is set from it on every non-prefix keystroke
(kbd.c:565-578). But `case CTRL_X:` flattens it:

```c
editor.cx_prefix = 1;
editor.cx_prefix_arg = prefix.supplied ? prefix.value : 0;   /* kbd.c:760 */
```

and the two consumers test `editor.cx_prefix_arg > 0`:
`macro_replay(fd, editor.cx_prefix_arg > 0 ? editor.cx_prefix_arg : 1)`
(kbd.c:434-435) and `cmd_eval_last_sexp(editor.cx_prefix_arg > 0)`
(kbd.c:439). So `C-u 0 C-x e` replays the macro **once** instead of zero times
(Emacs runs it zero times), and `C-u 0 C-x C-e` does not insert (Emacs treats a
zero prefix as supplied and non-nil, so it inserts).

`macro_replay()` (macro.c:73, `while (count-- > 0 && running)`) already handles
`count == 0` correctly, so only the plumbing is wrong.

### The cheap fix first

`editor.current_prefix` is **not** cleared by the `C-x` keystroke and the
follow-up key returns early at kbd.c:551-555 before it is reassigned, so
`handle_cx_prefix_key()` can already read `editor.current_prefix` and see the
true `{supplied, value}`. The minimal, correct change is therefore to delete
`editor.cx_prefix_arg` (def.h:278, kbd.c:760) and read `editor.current_prefix`
at kbd.c:435 and :439:

```c
struct command_prefix p = editor.current_prefix;
macro_replay(fd, p.supplied ? p.value : 1);
...
cmd_eval_last_sexp(p.supplied);
```

Verify the same reasoning for the `C-c` path (`handle_cc_prefix_key()`,
kbd.c:310) and the `C-x r` path (`handle_rect_prefix_key()`, kbd.c:334): both
also run with `current_prefix` still holding the prefix typed before the prefix
key. If that holds, `C-c` gains prefix support for free; state it explicitly in
a comment so a later refactor does not silently break it.

### Optional hardening

If review prefers explicitness over the implicit survival of
`current_prefix`, add helpers rather than more integer fields:

```c
static struct command_prefix editor_take_prefix(void);
static void editor_clear_prefix_state(void);
```

Rules: a command consumes the prefix exactly once; cancellation (`C-g`,
kbd.c:166-173) clears it; `C-u 0` stays supplied with value zero; the
`PREFIX_ARG_MAX` cap (kbd.c:35, 1000) and the negative-prefix policy are
unchanged; keyboard, macro replay, M-x
(`cmd_execute_named_with_prefix()`, cmd.c:867) and Lisp command execution
(lisp.c:1288, which passes `(struct command_prefix){ 0, 0 }`) all receive the
same structure. Avoid `value > 0` as a proxy for presence anywhere.

### Tests

Native: `test/fuzz_keypress.c` already drives `editor_process_keypress()` with
the full `kbd.o` — a small native key-state test can do the same, or extend the
fuzzer's invariants. Table over absent / 0 / 1 / 4 / `PREFIX_ARG_MAX` across a
direct command, a `C-x` command, a `C-c` binding, an M-x command, macro
execution, and cancellation at each prefix state.

PTY (copy the shape of `test/pty/macro-prefix-key-reapat-n-times.yaml`, which
already records a macro with `C-x (` … `C-x )` and replays it with
`C-u 2 C-x e`):

- `C-u 0 C-x e` performs zero replays — `expected_saved` equals the buffer
  before the replay;
- `C-u 0 C-x C-e` follows whatever zero behaviour you document (state it in
  `doc/kg.1`; Emacs inserts);
- the next unrelated command receives no stale prefix.

## Phase 6 — Add a stateful edit/undo reference model

### Files

New `test/test_edit_model.c` (add to `TESTBINS` with an
`EXTRA_edit_model := $(TESTDIR)/stubs_buffer.o … $(TEST_SRCS_OBJS)` line
mirroring `EXTRA_buffer`) or a libFuzzer target next to `test/fuzz_keypress.c`;
`Makefile`; `doc/FUZZING.md`.

### Model

Maintain a flat byte string, point, mark, a saved snapshot, and an operation
history. Generate small sequences of: insert valid/malformed glyph;
Backspace/Delete; newline/join; overwrite/transpose; set/exchange mark;
kill/yank; undo; save checkpoint.

After every operation compare: flattened kg rows (via
`editor_rows_to_string()`, buffer.c:415) against the reference bytes; row count
and the `chars[size] == '\0'` invariant on every row; point/mark boundary
policy; `editor.dirty` truth against the snapshot; and undo restoration.

Use exhaustive enumeration for very small strings and sequences, then reuse the
same model in libFuzzer. Seed with ASCII, 2/3/4-byte glyphs, combining marks,
wide glyphs, stray continuations, and truncated leads — the same corpus Phase 1
uses.

## Commit sequence

1. Tests demonstrating the UTF-8 deletion failures.
2. Span deletion fix (Phase 1).
3. EOL `kill-line` undo test and fix (Phase 2).
4. Undo eviction test and minimal `clean_size` fix (Phase 3).
5. Stable clean-state identity follow-up (Phase 3, optional).
6. Row termination audit and fix (Phase 4).
7. Prefix-state fix and tests (Phase 5).
8. Stateful model target (Phase 6).

Every commit must leave `make check` green.

## Final verification

```sh
make check
make WITH_LISP=0 clean all check
make fuzz-keypress-smoke
.ci/run-ci-steps.sh --parallel
```

`WITH_LISP=0` matters here: `cmd_eval_last_sexp()` is only reachable when
`kg_lisp_active()` (kbd.c:438), and `src/lisp.c` is one of the
`struct command_prefix` producers. Both configurations must stay green.

Do not raise `SCC_COMPLEXITY_MAX` (Makefile, currently 4208) or
`PMCCABE_FUNCTION_COMPLEXITY_MAX` (currently 120, "lower it, do not raise it")
merely because state was centralised. Split helpers so the existing ratchets can
fall or stay level.
