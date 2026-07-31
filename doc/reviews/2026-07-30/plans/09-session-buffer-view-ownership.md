# Plan 09 — Session, buffer, view, and window ownership

## Status (2026-07-31)

Phases 0-5 are **landed**; phases 6 and 7 are **deferred**.  See
[Landed / deferred](#landed--deferred) at the end of this document for
what changed against the plan as written and where the next implementer
picks up.

## Goal

Give every piece of persistent editor state exactly one owner, delete the
manual copy protocols that keep three overlapping records in sync, and give
buffers a stable identity that outlives a slot.

This is the first large refactor. It must not add a user-facing feature.

## Verified starting point

Verified against `src/` at `906e48f`. Re-check the line numbers before you
start; the names are what matter.

### Three overlapping records, plus loose globals

- `struct editor_config editor` — `src/def.h:259-324`, defined `src/main.c:46`.
  Holds terminal geometry, view point/scroll, the *current* buffer's text and
  filename, marks, local options, prefix/input state, and status/echo state.
- `struct editor_buffer buflist[MAX_BUFFERS]` (20) — `src/def.h:392-424`,
  defined `src/bufmgr.c:36`. Duplicates 26 of `editor`'s fields plus `active`
  and a per-slot `struct undo_stack`.
- `struct editor_window winlist[MAX_WINDOWS]` (8) — `src/def.h:378-388`,
  defined `src/winmgr.c:7`. Duplicates `cx/cy/rowoff/coloff/rowoff_visual`
  and holds geometry plus a raw `bufidx`.
- Free-standing globals: `undostack` (`src/undo.c:10`), `killring`
  (`src/yank.c:11`), the rectangle kill ring (`src/rect.c:12-14`),
  `suppress_undo`/`running`/`global_auto_revert`/`electric_pairs`
  (`src/main.c:47-51`), `win_total_rows`/`win_total_cols`
  (`src/winmgr.c:10-11`), `g_compilation` (`src/compile.c:16`).

There is no `session`, no `kg_buffer`, and no handle type today; every name in
the target section below is new.

### Five sync protocols and one stray partial write

1. `buf_save_to_slot(int)` — `src/bufmgr.c:46-86`, `static`. 28 assignments
   including `b->undostack = undostack` (a struct copy that *moves* heap
   ownership). 8 call sites.
2. `buf_restore_from_slot(int)` — `src/bufmgr.c:89-140`, exported via
   `src/def.h:505`. The inverse, **minus `readonly`** (recomputed by
   `editor_refresh_readonly_state()`), **plus** three side effects: it sets
   `buf_current`, rewrites `winlist[win_current].bufidx`, and may
   `silent_revert_current()`. 11 call sites, including `src/winmgr.c:40`,
   `src/winmgr.c:376` and `src/compile.c:272`.
3. `win_save_active_view()` — `src/winmgr.c:15-33`. Writes five view fields to
   the window slot **and again** to the buffer slot.
4. `win_restore_active_view()` — `src/winmgr.c:47-58`, plus the private
   `win_activate_window()` (`src/winmgr.c:37-42`) that chains
   `buf_restore_from_slot()` and it. Also overwrites
   `editor.screenrows/screencols` from window geometry.
5. `struct editor_buffer_swap` + `buf_temp_swap_in/out()` —
   `src/bufmgr.c:1682-1749`. A 9-field partial swap used by
   `buf_append_special_text()`, `buf_clear_special_text()` and
   `buf_truncate_last_row()` so background producers can mutate a
   non-current buffer through the current-buffer row primitives.
6. `editor_set_syntax()` (`src/syntax.c:1878-1886`) writes `editor.syntax`
   *and* `buflist[buf_current].syntax`, outside every protocol above.

`buf_save_current_state()` (`src/bufmgr.c:143-147`) pairs (3) with (1) and is
what most switch paths actually call.

### The "is it current?" ternary

Because `editor.*` is authoritative only for the current buffer, every reader
that may touch a non-current buffer repeats the same conditional:

- `src/display.c:475-497` (rows, numrows, visual-line flag; its comment
  explains `b->row` is a stale pointer after a realloc);
- `src/bufmgr.c:298` (auto-revert filename);
- `src/winmgr.c:412-413` (`win_position_at_end`);
- `src/bufmgr.c:1832-1924` (`buf_append_special_text` window follow logic).

Deleting this ternary is the observable success criterion for the plan.

### Asymmetries already in the tree

- `editor.desired_visual_col` is in **neither** `editor_buffer` nor
  `editor_window`, so the goal column survives a buffer or window switch. Fix
  it in the view phase; characterize it first.
- `editor.screenrows/screencols` are window geometry stored on the session
  record and rewritten by `win_reflow()`/`win_restore_active_view()`.
- `editor.readonly` is derived, not stored, on restore only.
- `recenter_state`, `window_line_state`, `last_key`, `paste_mode`,
  `echo_cursor_col`, `statusmsg*` and the prefix fields are session-scoped and
  correctly *not* copied — leave them until the last phase.

### References that outlive one command

- `struct editor_window.bufidx` — a bare slot index (`src/def.h:379`).
- `struct compilation_state.source_buffer`, `.compilation_buffer`,
  `.pending_source_buffer` — bare slot indices (`src/compile.h:24-25,50`) held
  for the whole life of an async compile. `buf_kill()` (`src/bufmgr.c:1585`)
  clears `active` and `buf_prepare_special_text()` (`src/bufmgr.c:1787`) hands
  the same slot to an unrelated buffer, so these can silently retarget.
- `src/kbd.c:1117` uses the **`editor.filename` pointer value** as a proxy for
  "same buffer as before the command" — the clearest argument for
  `(id, generation)`.

### Existing coverage to build on

- Native: nothing exercises buffer switching or windows. `test_buffer.c` links
  `bufmgr.o`/`fileio.o`/`dired.o`/`compile.o` (`Makefile:333`) but only tests
  rows, offsets and file IO. `test/stubs_buffer.c` stubs the three `win_*`
  entry points; a new window test replaces those stubs with `winmgr.o`.
- PTY (224 cases): `C-x 2` once (`99-resize-split`), `C-x 3` once
  (`visual-line-mode-window-local`), `C-x o` once (`90b-kill-compilation`),
  and **zero** cases for `C-x b`, `C-x C-b`, `C-x k`, `C-x 0`, `C-x 1`.
  Two windows on one buffer is untested end to end.

## Target ownership

```c
struct kg_buffer {
	uint32_t id;		/* 0 = free slot; never reused */
	uint32_t generation;	/* bumped on kill and on slot reuse */
	bool active;
	erow *rows;
	int row_count;
	int row_capacity;	/* Plan 08 phase 1; add the field now */
	char *filename;
	struct editor_syntax *syntax;
	int dirty;
	uint64_t content_generation;	/* Plan 10 uses this */
	struct undo_stack undo;
	struct file_snapshot disk;	/* mtime, size, changed, auto_revert */
	struct mark_state marks;	/* mark + ring + region flags */
	struct buffer_options options;	/* readonly*, compile_command*,
					   visual_line_mode, overwrite_mode */
	struct kg_point last_point;	/* seed for a window's first visit */
};

struct kg_view {
	struct kg_buffer_handle buffer;
	int cx, cy;
	int rowoff, coloff, rowoff_visual;
	int desired_visual_col;
};

struct kg_window {
	bool active;
	struct kg_view view;
	int x, y, w, h;
	int col_group;
};

struct kg_session {
	struct editor_terminal terminal;	/* rows, cols, rawmode, paste */
	struct command_state commands;		/* prefix, repeat, last_key */
	struct minibuffer_state echo;		/* statusmsg, echo_cursor_col */
	struct kill_ring killring;
	struct kg_buffer buffers[MAX_BUFFERS];
	struct kg_window windows[MAX_WINDOWS];
	int current_buffer;
	int current_window;
};
```

Names may follow project style. Ownership must follow this shape. Note there
is no `struct process_manager` here: generalizing the single `g_compilation`
belongs to Plan 12, not this plan.

## Invariants (hold after every phase, not only at the end)

1. Buffer text, filename, dirty, undo, disk snapshot, local options, syntax
   and marks have exactly one owner: the buffer slot.
2. Point, scroll and goal column for a displayed buffer have exactly one
   owner: the window's view.
3. Any reference that outlives one command holds `(id, generation)`, never a
   bare slot index.
4. `current_buffer` is the selected window's view buffer, by construction.
5. Two windows on one buffer share text, undo and options and have
   independent point/scroll.
6. Killing or reusing a slot bumps `generation`; stale handles resolve to
   `NULL` and callers must handle it.
7. Background producers edit a named target buffer and never make it current.
8. **No field is writable through two names across a commit boundary.**

## The flag-day technique

Moving `editor.row` alone touches 378 references; `numrows` 253, `cx` 108,
`rowoff` 109, `mark_*` 139. There is no way to migrate those a module at a
time without a period where both `editor.row` and `buflist[i].row` are live
and writable — the exact failure this plan exists to remove.

So each ownership move is a **single scripted flag-day commit**:

1. Add the accessor first, in its own commit:
   `static inline struct editor_buffer *bcur(void)` returning
   `&buflist[buf_current]` (later `wcur()` for the window). No behavior change.
2. In the flag-day commit, apply a pure textual substitution
   (`editor.row` → `bcur()->rows`, etc.) across `src/` and `test/`, **delete
   the field from `struct editor_config` in the same commit**, and let the
   compiler enumerate anything the script missed.
3. Hand edits in that commit are restricted to a list named in the phase
   below. Everything else must be reproducible by rerunning the script.
4. Review the diff as two passes: `git diff -w` to confirm the mechanical
   hunks are uniform, then read only the hand-edited functions.
5. `make check`, `make WITH_LISP=0 clean all check`, and the Phase 0
   characterization suite under ASan must pass in the same commit.

Later phases then replace `bcur()`/`wcur()` with explicit
`struct editor_buffer *` / `struct kg_view *` parameters **one module at a
time**, which is safe and incremental precisely because there is already only
one owner.

## Phase 0 — Characterize before changing anything

### Files

- new `test/test_winmgr.c`, `Makefile` (`TESTBINS`, `EXTRA_winmgr`)
- `test/stubs_buffer.c` (drop the window stubs for this binary)
- new PTY cases under `test/pty/`

### Native model

Link `bufmgr.o`, `winmgr.o`, `buffer.o`, `undo.o`, `fileio.o`, `syntax.o`,
`width.o` against a stub set modelled on `test/stubs_buffer.c` but **without**
its `win_save_active_view`/`win_display_buffer_other_window`/
`win_position_at_end` stubs. Assert after each step: which slots are active,
which pointer each slot and `editor` hold, `buf_current`/`win_current`,
per-slot `dirty` and undo size. Sequence:

- open A, edit, mark, set read-only / local compile-command;
- open B, switch back and forth, assert no field crossed over;
- `C-x 2`, `C-x 3`, same buffer in two windows, move independently;
- show a different buffer in the other window;
- `C-x o`, `C-x 0`, `C-x 1`;
- kill a buffer that is visible in two windows; kill the current buffer;
- fill all 20 slots, kill one, reopen — record which slot is reused;
- `buf_append_special_text()` into a hidden buffer while editing another,
  then into one visible in a second window;
- reflow at 24x80, 10x40 and a 3-row window.

### PTY gaps to fill (they are the regression net for phases 2-5)

- `C-x b` round trip: point, mark, dirty flag and undo depth survive.
- `C-x C-b`, `RET` on a listing row, `q`.
- `C-x k` on a buffer visible in two windows (tmux, `expected_screen_*`).
- `C-x 2` then edit in one window and check the other window's point in the
  rendered frame.
- goal column across `C-x b` — this currently leaks
  (`editor.desired_visual_col`). Land the case as `xfail: true` if the
  current behavior is judged wrong, and clear it in Phase 5.

Phase invariant: **no production file changes.** Keep this suite green for the
rest of the plan; a phase that needs it edited must say why in the commit
message.

## Phase 1 — Stable buffer identity

### Files

`src/def.h`, `src/bufmgr.c`, `src/compile.c`, `src/compile.h`, tests.

### Changes

```c
struct kg_buffer_handle { uint16_t slot; uint32_t id; uint32_t generation; };

struct editor_buffer *buf_resolve(struct kg_buffer_handle);
struct kg_buffer_handle buf_handle(int slot);
```

- `id` is assigned from a monotonic counter in `buf_reset_slot()`
  (`src/bufmgr.c:1751`) and the `buf_reset()`/`buf_save_to_slot()` open paths;
  `0` means "free".
- `generation` increments in `buf_kill()` and in every path that hands a slot
  to a different buffer.
- `buf_resolve()` returns `NULL` for out-of-range, inactive, or mismatched
  `id`/`generation`, and counts the failure in a debug counter.
- Convert `compilation_state.source_buffer`, `.compilation_buffer` and
  `.pending_source_buffer` to handles. Every use re-resolves; a `NULL` result
  ends the compile cleanly instead of writing into a recycled slot.

Leave `editor_window.bufidx` a slot index for now — it is re-read every frame
and Phase 5 replaces the whole record.

Phase invariant: every retained cross-command buffer reference outside
`winlist` is a handle, and resolving a killed or reused slot fails loudly.

## Phase 2 — Buffer owns its text (flag day A)

### Fields moved

`row`, `numrows`, `dirty`, `syntax`, and the undo stack. They move together
because the row primitives couple them: `editor_insert_row()` writes
`editor.row`/`numrows`/`dirty` and reaches `editor_update_syntax()`, which
reads `editor.syntax` and walks `editor.row` for downstream propagation.

### Hand-edited functions in this commit (the whole list)

- `src/buffer.c`: `editor_insert_row`, `editor_del_row`, `editor_free_all_rows`
  and the local `editor_row_append_raw` in `src/bufmgr.c:1761` gain a leading
  `struct editor_buffer *`.
- `src/syntax.c`: `editor_update_row`, `editor_update_syntax`,
  `editor_rehighlight_from`, `editor_rehighlight_all`, `editor_set_syntax` gain
  the owning buffer; `editor_set_syntax`'s duplicate write to
  `buflist[buf_current].syntax` disappears.
- `src/undo.c`: `undo_init`, `undo_free`, `undo_push`, `editor_undo`,
  `undo_mark_clean` take `struct editor_buffer *`; the global `undostack`
  (`src/undo.c:10`) and its `extern` (`src/def.h:432`) are deleted.
- `src/bufmgr.c`: `buf_append_special_text`, `buf_clear_special_text`,
  `buf_truncate_last_row` operate on the resolved target buffer;
  `struct editor_buffer_swap` and `buf_temp_swap_in/out` are **deleted**.
- `src/bufmgr.c`: the four `row`/`numrows`/`dirty`/`syntax` lines drop out of
  `buf_save_to_slot`/`buf_restore_from_slot`.
- `src/display.c:475-497` loses the rows/numrows ternary.

Everything else is `editor.row` → `bcur()->rows` and friends.

Phase invariant: there is exactly one live `erow *` per buffer and exactly one
undo stack per buffer; `rg 'editor\.(row|numrows|dirty|syntax)\b'` and
`rg '\bundostack\b'` return nothing; no code temporarily impersonates another
buffer.

Do **not** change undo semantics, opcode set, or dirty counting here. That is
Plan 10.

## Phase 3 — Buffer owns filename, disk snapshot and options (flag day B)

### Fields moved

`filename`, `disk_mtime`, `disk_size`, `disk_changed`, `auto_revert`,
`readonly`, `readonly_local`, `readonly_override`, `compile_command`,
`compile_command_user_override`, `visual_line_mode`, `overwrite_mode`.

Group them as `struct file_snapshot` and `struct buffer_options` while moving;
that is cheaper than moving them flat and regrouping later.

Hand edits: `editor_refresh_readonly_state`, `editor_set_local_readonly`,
`editor_set_readonly_override` (`src/buffer.c`), `buf_apply_local_settings`
(`src/bufmgr.c:1170`), `editor_snapshot_disk`/`file_state_differs`
(`src/fileio.c`), `autorevert_poll` (drops the `src/bufmgr.c:298` ternary),
`src/kbd.c:1117` — which now compares `(id, generation)` instead of the
filename pointer.

Phase invariant: no file identity or per-buffer option is readable from the
session record; `write_slot()` (`src/bufmgr.c:1516`) and the interactive save
path read the same fields.

## Phase 4 — Buffer owns marks (flag day C)

### Fields moved

`mark_set`, `mark_row`, `mark_col`, `mark_highlight`, `mark_ring_row[]`,
`mark_ring_col[]`, `mark_ring_len`, `shift_select`, `rect_mode`.

Decide and document, with a test each:

- the mark ring is buffer-owned (matches Emacs and matches today's copy);
- `mark_highlight`, `shift_select` and `rect_mode` are region *presentation*.
  kg keeps one active region per buffer for now — write that down in
  `README.md` and pin it with a two-window PTY case rather than letting it
  become an accident of which struct the field landed in.

Marks are still `(row, col)` pairs that are **not relocated when text
changes** — verified: nothing in `src/` adjusts `mark_row`/`mark_col` on an
edit. Do not fix that here; Plan 10 phase 5 replaces them with markers.

After this phase `buf_save_to_slot` and `buf_restore_from_slot` have no
content left to copy. Delete `buf_save_to_slot`, and replace
`buf_restore_from_slot(idx)` with `buf_select(int slot)` that only sets
`buf_current`, retargets the selected window and runs the auto-revert check.
Delete `buf_save_current_state()`.

Phase invariant: `rg 'buf_save_to_slot|buf_restore_from_slot'` is empty and
`struct editor_config` contains no buffer-scoped field.

## Phase 5 — Window owns the view (flag day D)

### Fields moved

`cx`, `cy`, `rowoff`, `coloff`, `rowoff_visual`, `desired_visual_col`
(currently owned by nobody), and the derived `screenrows`/`screencols`, which
become reads of `w->h`/`w->w`.

### Changes

- `struct kg_view` is embedded in `struct kg_window`; `bufidx` becomes a
  `struct kg_buffer_handle`.
- Introduce `wcur()`, `editor_current_window()`, `editor_current_view()`,
  `view_buffer()`.
- Delete `win_save_active_view()`, `win_restore_active_view()` and
  `win_activate_window()`. `win_cycle_next`/`win_delete_current` become
  "change `win_current`, then reflow".
- `win_split_horizontal`/`win_split_vertical` copy the *view*, which is the
  correct semantic and is already what `winlist[slot] = winlist[win_current]`
  does.
- Selecting a buffer in a window: resolve the handle; if this window has shown
  the buffer before, restore its own view; otherwise seed from
  `buffer->last_point`; clamp through **one** helper — fold
  `clamp_cursor_to_buffer()` (`src/bufmgr.c:153`) and the ad-hoc clamps in
  `win_reflow()` (`src/winmgr.c:152-164`) into it.
- `buffer->last_point` is updated when a view detaches from a buffer.
- `win_position_at_end()` and the `buf_append_special_text()` follow logic lose
  their `win_current` special cases and become plain loops over views.

Phase invariant: the selected window's view is the only writable point/scroll;
`rg 'editor\.(cx|cy|rowoff|coloff|screenrows|screencols)'` is empty; two
windows on one buffer move independently in the Phase 0 native model and in
the tmux PTY case.

## Phase 6 — Nest the session record

Only now rename `struct editor_config editor` to `struct kg_session` and nest
`terminal` / `commands` / `echo` / `killring`. Doing it last keeps the risky
flag days from being buried in rename noise, and by this point the struct is
about a dozen fields. `win_total_rows`/`win_total_cols` move into
`session.terminal`; the rectangle kill ring joins `session.killring` or stays
module-private, but it must stop being bare file statics if Plan 13's kill
ring depends on it.

Phase invariant: `struct kg_session` contains no field that any buffer or
window also stores.

## Phase 7 — Lifecycle callbacks and accounting

C-only, synchronous, at top-level points where no mutation is partial: buffer
created, buffer about to be killed, buffer killed (generation bumped), view
attached, view detached. Add debug counters behind the existing assert build:
active buffers/windows, generation bumps, failed handle resolutions, live
`erow *` owners. These are the hooks Plan 10's markers and Plan 12's processes
attach to; do not expose them to Lisp here.

## WITH_LISP=0

`src/lisp.c` reads `editor.row`, `editor.numrows`, `editor.mark_row/col` and
`buf_current` at roughly 20 sites (`src/lisp.c:421-898`). It is compiled out
under `WITH_LISP=0`, so a flag-day script that only compiles the default
configuration will silently leave `WITH_LISP=1` broken, or vice versa.

Rules:

- Every flag-day commit runs `make WITH_LISP=0 clean all check` **and**
  `make check` before it is pushed.
- `test/stubs_buffer.c`, `test/stubs.c`, `test/stubs_noyank.c`,
  `test/stubs_extra.c` and `test/fuzz_stubs.c` all define or stub state this
  plan moves; they are part of every flag-day substitution, not an afterthought.
- `src/lisp.h` keeps its `KG_USE_LISP`-free shape. The Lisp adapter continues
  to translate 1-based character positions in `src/lisp.c` only.

## Not in this plan

- No marker relocation, edit transaction, or change events (Plan 10).
- No command/keymap/mode registry (Plan 11); modes stay syntax-pointer identity.
- No generalized process manager, no Lisp-visible handles or buffer-local
  variable API (Plan 12).
- No new user-facing command or keybinding.
- No performance work; if a flag day makes a hot path obviously worse, note it
  for Plan 08 rather than fixing it inline.

## Test matrix

Run for `WITH_LISP=0` and `1`, and under ASan and Valgrind:

- buffer switch round trip: point, mark, undo depth, dirty, read-only,
  compile-command, visual-line-mode;
- two windows on one buffer (independent point, shared text/undo); two
  windows, two buffers;
- kill a buffer displayed twice; kill the current buffer; kill the last buffer;
- slot exhaustion, kill, reopen, stale handle resolution;
- background append to a hidden and to a visible special buffer; compilation
  output while the source buffer is killed mid-run;
- Dired refresh, `*help*`, `*Buffer List*`, `*scratch*`;
- auto-revert of the current and of a hidden buffer;
- resize to 24x80, 10x40, and a window too small to split.

Outcome-only tests pass through ownership bugs. Assert pointers and counters in
the native model, not just saved bytes.

## Migration rules

- One field family per commit; never two writable names for one field.
- Before deleting a field, `rg` its exact name across `src/` **and** `test/`.
- Never store a raw `erow *` or slot index in anything that outlives a command.
- Re-read every free/reset path when a pointer changes owner: `buf_kill`,
  `editor_cleanup` (`src/bufmgr.c:2122`), `buf_reset_slot`,
  `free_load_result`/`commit_load_result` (`src/fileio.c`), `undo_free`.
- Keep commits behavior-neutral until Phase 5; the goal-column leak is the
  single intended behavior change and it gets its own commit and test.

## Acceptance

```sh
make check
make WITH_LISP=0 clean all check
.ci/run-ci-steps.sh --parallel
```

Exit criteria:

- `buf_save_to_slot`, `buf_restore_from_slot`, `buf_save_current_state`,
  `win_save_active_view`, `win_restore_active_view`, `editor_buffer_swap`,
  `buf_temp_swap_in`, `buf_temp_swap_out` and the global `undostack` are gone;
- no `x == buf_current ? editor.f : b->f` ternary remains;
- `struct editor_config`/`kg_session` holds no buffer- or view-scoped field;
- every cross-command buffer reference is `(id, generation)`;
- the Phase 0 native model and the new window/buffer PTY cases pass under
  ASan and Valgrind in both Lisp configurations.

## Landed / deferred

### Landed

| Phase | State | Commits |
| --- | --- | --- |
| 0 — characterize | complete | `a21cfbe` (native model `test/test_winmgr.c`, `test/stubs_win.c`), `408f3cd` (stub fix it exposed) |
| 1 — stable buffer identity | complete | `52fd089` |
| 2 — buffer owns its text (flag day A) | complete | `12c041d` (accessor), `be1f45d` (flag day), `40c6cd5` (analyzer follow-up) |
| 3 — filename, disk, options (flag day B) | complete | `4556cf8` |
| 4 — marks (flag day C) | complete | `88edf96`, `3e83325` (coverage re-baseline) |
| 5 — window owns the view (flag day D) | complete | `71d7caa` (accessor), `e16710a` (flag day), `1e26dcc` (fuzz harness + coverage), `861dbe2` (invariant 4 test) |

All six copy protocols and the global undo stack are gone:

```sh
rg 'buf_save_to_slot|buf_restore_from_slot|buf_save_current_state' src/ test/
rg 'win_save_active_view|win_restore_active_view|win_activate_window' src/ test/
rg 'editor_buffer_swap|buf_temp_swap_(in|out)' src/ test/
rg '(^|[^.>_[:alnum:]])undostack\b' src/ test/   # only the struct field
rg 'editor\.(row|numrows|row_capacity|dirty|syntax|filename|disk|readonly|compile_command|mark_|shift_select|rect_mode|cx|cy|rowoff|coloff|screenrows|screencols|desired_visual_col)' src/ test/
```

all return nothing.  `struct editor_config` holds the terminal mode, the
echo area, the prefix/command state and two command cycle states —
session state, all of it.

### Deviations, all deliberate

- **Phase 3 moved the file/option fields flat** rather than regrouping
  them into `struct file_snapshot` / `struct buffer_options`.  The plan
  judged grouping cheaper than moving flat and regrouping later; that was
  written before the fields turned out to already exist in `struct
  editor_buffer` under the same names, which makes the move a pure
  substitution and the grouping an independent rename of two structs.
  Still worth doing — as a cleanup, not as part of a flag day.
- **`winlist[].bufidx` stays a slot index.**  Every window is walked and
  re-read on every frame, so a window outliving its buffer is a display
  bug rather than a stale write.  Give it a handle when the window record
  grows a lifecycle (phase 7), not inside a substitution.
- **`clamp_cursor_to_buffer()` and `win_reflow()`'s clamps are not
  folded.**  They answer different questions ("is point on a row that
  exists?" and "is point inside the window?"), so folding them changes
  behaviour, which a flag day must not.
- **`undo_push()` took `struct editor_buffer *` in phase 2, not just
  `bcur()`.**  gcc `-fanalyzer` cannot see that a store through a pointer
  into an extern array escapes and read every push as a leak; a parameter
  is clean, and it is the signature the plan wanted anyway.
- **The plan's phase-2 hand-edit list did not mention the highlighter
  callback.**  `struct editor_syntax.highlight` had to take the buffer:
  multi-line state comes from the row above, which is only reachable
  through the owning buffer, and with `bcur()` a hidden `*compilation*`
  buffer would have read past the end of somebody else's row array.

### Deferred

**Phase 6 — nest the session record.**  Not started.  `struct
editor_config editor` is not renamed to `struct kg_session` and
`terminal` / `commands` / `echo` / `killring` are not nested;
`win_total_rows`/`win_total_cols` and the rectangle kill ring are still
bare globals.  This is a rename, and the plan puts it last precisely
because it is noise; the struct is now about a dozen fields, which is the
size the plan predicted it would be at this point.  Nothing else depends
on it.

**Phase 7 — lifecycle callbacks and accounting.**  Not started, with one
exception: `KG_PERF_HANDLE_STALE` landed in phase 1 and counts handles
that outlived the buffer they named.  The buffer-created /
about-to-be-killed / killed / view-attached / view-detached hooks do not
exist.  `buf_claim_slot()`, `buf_kill()`, `buf_attach_view()` and
`buf_remember_view()` are the five points they attach to, and each is now
a single named function, which is the thing that made this phase cheap to
defer.

**Where the next implementer starts.**  Either deferred phase can be done
independently and in either order.  Phase 7's handle conversion for
`winlist[].bufidx` should go with its lifecycle hooks.  Plan 10 depends
on phases 2-4, which are landed, so it is not blocked by either.
