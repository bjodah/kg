# Plan 10 — Failure-atomic edit transactions, markers, decorations, and hooks

## Goal

Create one mutation gateway that owns undo, dirty state, marker relocation,
render/syntax invalidation, and deferred change notification.

Depends on: the release fixes in Plans 01-04 (especially the undo fixes in
Plan 02, which this plan must not silently redo); at least Plan 09 phases 2-4,
so a buffer owns its rows, dirty counter and undo stack; optionally Plan 08
phase 5's `editor_row_replace_range()`, which is the local precursor to the
transaction.

## Verified starting point

Verified against `src/` at `906e48f`.

### The undo layer today

- One global `struct undo_stack undostack` (`src/undo.c:10`) plus a per-slot
  copy in `struct editor_buffer.undostack`, moved by struct assignment in
  `buf_save_to_slot`/`buf_restore_from_slot`. Plan 09 phase 2 deletes the
  global; this plan assumes that has happened.
- `undo_push(type, row, col, c, text, len)` (`src/undo.c:39-110`) —
  **45 call sites** in `src/`: `buffer.c` 15, `word.c` 13, `search.c` 4,
  `yank.c` 3, `kbd.c` 3, `cmd.c` 2, `rect.c` 2, and one each in `fileio.c`,
  `lisp.c`, `shell.c`.
- 11 opcodes (`src/def.h:341-353`), all addressed by `(row, col)` **int**
  pairs, not byte offsets: `INSERT_CHAR`, `DELETE_CHAR`, `INSERT_LINE`,
  `DELETE_LINE`, `SPLIT_LINE`, `JOIN_LINE`, `KILL_TEXT`, `YANK_TEXT`,
  `REPLACE_TEXT`, `REFLOW_PARA`, `RECT_OVERWRITE`. The last three are
  already ad-hoc "restore this span" records.
- Singly-linked LIFO, `MAX_UNDO_SIZE` 1000, eviction walks the list
  (`src/undo.c:83-108`; Plan 08 phase 7 fixes the walk).
- **There is no redo.** `editor_undo()` (`src/undo.c:113-336`) pops and frees.
- Clean state is `undostack.size == undostack.clean_size`
  (`src/undo.c:325-327`, `undo_mark_clean` at `:339`).

### `suppress_undo` is two different mechanisms

`int suppress_undo` (`src/main.c:49`) has 43 references and about 18
save/restore regions. They mean two unrelated things:

- *internal load / rebuild*: `bufmgr.c:228-230` (reload), `bufmgr.c:1829-1926`
  (special-buffer append), `yank.c:460-503`, `buffer.c:1000-1046`
  (`editor_insert_text_raw`);
- *undo replay reentrancy guard*: `undo.c:261-283` and `undo.c:295-318`, where
  `RECT_OVERWRITE` and `REFLOW_PARA` replay by calling row primitives that
  would otherwise record new undo records;
- *"I will push my own coarse record instead"*: `rect.c` (four regions),
  `search.c:166-174,706-714,935-943`, `word.c:652-654,870-…`, `kbd.c:689-701`.

Do not replace all three with one flag. The transaction gets an explicit
option for the first, a distinct replay mode for the second, and grouping
(phase 4) for the third.

### Dirty is a counter, and it is used as a change detector

`editor.dirty` is an `int` incremented once per *row-level* operation
(38 `dirty++` sites), so a single command already bumps it many times. Two
places read the counter as a change signal:

- `src/kbd.c:98-100` — `editor_insert_repeated_literal` compares
  `dirty != dirty_before` to decide whether to push per-character undo records;
- `src/kbd.c:513,1117` — `editor_process_keypress` deactivates the region when
  `editor.filename == fname_before && editor.dirty != dirty_before`, i.e. it
  uses the filename *pointer* as buffer identity and the dirty counter as a
  content-change flag.

Both must move to `(buffer handle, content_generation)` when the transaction
lands; otherwise "one dirty bump per transaction" silently changes when the
region is deactivated. Add a PTY case for region deactivation *before*
touching either site.

### Invalidation today

- `editor_update_row()` rebuilds one row's `render` and `hl`;
  `editor_update_syntax()` then calls `syntax_propagate_downstream()`.
- `editor_rehighlight_from(int)` (`src/syntax.c:1849-1855`) re-highlights a
  single row plus propagation, despite the name.
- `editor_rehighlight_all()` walks every row; `editor_set_syntax()` calls it.
- There is **no frame-level dirty-region tracking**: `editor_refresh_screen()`
  unconditionally redraws every window. "Invalidate once per affected range"
  therefore means *syntax/render* work, not screen damage. Do not invent a
  damage list in this plan.

### Existing position conversions — do not create a second dialect

`src/buffer.c` already has `editor_char_offset(row, col)`,
`editor_offset_to_rowcol(long, int *, int *)` and
`editor_buffer_char_length()`. They are **0-based codepoint** offsets in
`long`, used only by `src/lisp.c` (which adds 1 for Emacs-style point).

The transaction should be **byte**-addressed internally. The rule: byte
offsets in `size_t` inside `buffer.c`/`marker.c`; the character-offset helpers
survive as the Lisp adapter and get reimplemented on top of the byte layer;
nothing outside `src/lisp.c` uses character offsets. Write that rule into
`src/def.h` next to the declarations, or the two dialects will interleave.

### Marker-like state that exists today

Nothing is relocated when text changes — verified: no site in `src/` adjusts
`mark_row`/`mark_col` after an edit.

- point/mark: `mark_set`, `mark_row/col`, `mark_highlight`, and the 16-entry
  ring (`MARK_RING_MAX`, `src/def.h:250`), pushed/popped in `src/yank.c`.
- isearch: `saved_cx/cy/rowoff/coloff` and `last_match_row/col`
  (`src/search.c:393-399`) held as ints across an interactive loop.
- `saved_hl` / `saved_hl_line` and the `RESTORE_HL` macro
  (`src/search.c:22-30`): a raw copy of `row->hl` written back with
  `memcpy(..., editor.row[saved_hl_line].rsize)`. It is keyed by row index and
  sized by the *current* `rsize`, so it is a live hazard if the row changes
  length between snapshot and restore. This is decoration consumer #1.
- compilation's retained buffer slots (`src/compile.h:24-25,50`) — Plan 09
  phase 1 converts these to handles.

## Core contract

```c
struct kg_edit {
	struct editor_buffer *buffer;
	size_t begin_byte;
	size_t end_byte;
	const char *replacement;
	size_t replacement_len;
	unsigned options;	/* KG_EDIT_NO_UNDO, KG_EDIT_REPLAY, ... */
};

struct kg_edit_result {
	size_t old_length;
	size_t new_length;
	uint64_t before_generation;
	uint64_t after_generation;
};

int kg_buffer_replace(const struct kg_edit *, struct kg_edit_result *);
```

The implementation translates flat byte positions to rows internally. Callers
outside `buffer.c` must not mutate row arrays.

## Invariants

1. Validate and reserve every allocation before publishing any mutation.
2. OOM or error leaves text, undo, dirty, markers and generations unchanged.
3. One logical edit produces one undo unit unless explicitly grouped.
4. All markers relocate according to documented gravity.
5. Render/syntax state is recomputed once per affected range, not per byte.
6. `content_generation` advances exactly once per committed transaction.
7. Runtime hooks never run inside row reallocation or a partial transaction.
8. Undo replay uses the same primitive with `KG_EDIT_REPLAY`, not raw
   shadow mutation and not the ambient `suppress_undo`.

## Phase 1 — Inventory and seal raw mutation

### Files

`src/buffer.c`, `src/def.h`, the 45 `undo_push` call sites, new
`utils/check_mutation_gateway.py`.

### Changes

Classify each of the 45 sites into: single-row replace; split/join; multi-row
replace; special-buffer rebuild; internal load (no undo); undo replay. Record
the classification as a table in the commit message — it is the migration order
for phase 9.

Narrow the exported row API in `src/def.h:570-604`. `editor_row_insert_char`,
`editor_row_insert_string`, `editor_row_insert_spaces`,
`editor_row_append_string`, `editor_row_del_char`, `editor_insert_row`,
`editor_del_row` become `static` in `buffer.c`, or move to a private
`src/buffer_internal.h` that only `buffer.c` and the transaction include.
Anything that cannot be made private yet gets an entry in a deny-list the
checker reads, and the deny-list may only shrink.

Add `KG_EDIT_NO_UNDO` and `KG_EDIT_REPLAY` as transaction options. Do not
remove `suppress_undo` yet; do not let new code set it.

Under an assert build, add a counter asserting that user-buffer content changed
only inside an active transaction.

Phase invariant: the set of modules that can mutate rows is enumerated and
non-growing. No behavior change.

## Phase 2 — Flat position translation

### Files

`src/buffer.c`, buffer struct, `test/test_buffer.c`.

### Changes

Unit: byte offset into the logical buffer, one byte per row separator.

```c
int buffer_position_to_row_col(
    const struct editor_buffer *, size_t pos, int *row, int *col);
size_t buffer_row_col_to_position(
    const struct editor_buffer *, int row, int col);
size_t buffer_byte_length(const struct editor_buffer *);
```

Use `size_t` with the existing `checked_*` helpers (`src/def.h:605-645`).
Reimplement `editor_char_offset`/`editor_offset_to_rowcol`/
`editor_buffer_char_length` on top and mark them "Lisp adapter only".

Define and test: empty buffer; position 0; EOF; kg's no-final-newline policy
for a buffer with a trailing empty row (`test_buffer_char_length_trailing_
empty_row` in `test/test_buffer.c` already pins the character-offset side of
this — mirror it); a position in the middle of a multi-byte glyph; malformed
UTF-8.

Scans may stay linear. Add a per-row prefix cache only after Plan 08 measures.

Phase invariant: two position dialects exist but only one is used outside
`src/lisp.c`, and round-tripping is property-tested.

## Phase 3 — Failure-atomic replace

### Files

`src/buffer.c`, `src/undo.c`, `src/def.h`, new `test/test_edit.c`.

### Preparation (nothing published)

- validate buffer active and writable unless `KG_EDIT_NO_UNDO`;
- validate `begin <= end <= buffer_byte_length()`;
- checked-calculate final byte and row counts;
- copy the deleted text for undo;
- allocate the new/changed row storage and the row-array growth;
- allocate the undo record and any group metadata;
- compute marker moves into a scratch array.

### Commit

- splice prefix / replacement / suffix;
- publish the new rows and capacities;
- apply the precomputed marker moves;
- recompute `render`/`hl` for the affected rows and propagate downstream once;
- append the undo unit;
- bump `content_generation`;
- recompute `dirty` from the clean-state identity;
- queue the change event;
- free the old storage.

One staged struct owns every pre-commit allocation and has one cleanup
function used by both the failure and the success path.

### Undo record

```c
struct undo_change {
	size_t position;
	char *old_text;
	size_t old_len;
	size_t new_len;	/* enough to reverse; there is no redo today */
};
```

Store `new_len` rather than `new_text` while redo does not exist, but store
the before/after generation identities so clean-state tracking is exact.
Adding redo later means adding `new_text` and nothing else.

Keep the 11 existing opcodes working alongside `undo_change` until phase 9
retires the last caller of each. `editor_undo()` gains a case for
`undo_change` that calls `kg_buffer_replace()` with `KG_EDIT_REPLAY`.

Phase invariant: the transaction exists and is used by at least one command;
every other path still works unchanged; OOM injection at each preparation
point leaves the buffer byte-identical.

## Phase 4 — Explicit edit grouping

```c
struct kg_edit_group *kg_edit_group_begin(
    struct editor_buffer *, enum edit_group_kind);
int kg_edit_group_commit(struct kg_edit_group *);
void kg_edit_group_abort(struct kg_edit_group *);
```

Rules:

- nested helper calls join the current group; there is no hidden nesting;
- abort before commit leaves the buffer unchanged;
- one undo reverses the whole group;
- the group carries initial point only where the command's semantics need it;
- **characterize before choosing**: today `query-replace` pushes one
  `UNDO_REPLACE_TEXT` per accepted replacement (`src/search.c`), and
  `97-sort-lines-undo`, `query-replace-smart-case-undo`,
  `git-rebase-drop-flag-undo`, `self-insert-utf8-undo` and `lisp-eval-c-j-undo`
  pin today's granularity. Decide per command whether to preserve or change it,
  and update those cases deliberately, one commit each.

Migrate in this order: reflow (`editor_reflow_paragraph`, already one coarse
record), sort lines, rectangle ops, shell-region replacement, query replace.
Lisp compound operations wait for Plan 12.

Phase invariant: every migrated command produces exactly one undo unit, and
its PTY undo case still describes the intended behavior.

## Phase 5 — Stable markers

### Files

new `src/marker.c` and declarations in `src/def.h`; buffer lifecycle; the
transaction; `Makefile`; `test/test_marker.c`.

```c
struct kg_marker {
	struct kg_buffer_handle buffer;
	size_t byte_position;
	enum marker_gravity gravity;
	bool active;
};
```

Store live markers in a per-buffer growable array; external consumers hold
marker IDs, not pointers. Relocation for a replace over `[begin, end)`:

- strictly before `begin`: unchanged;
- at or after `end`: shifted by the signed length delta;
- inside the deleted range: collapse to `begin` (left) or to end-of-insert
  (right) per gravity;
- exactly at `begin` with an empty deletion: gravity decides.

On buffer kill, detach and invalidate every marker; resolving a marker whose
handle is stale returns "gone", never a position.

Convert consumers only after property tests pass, in this order:

1. isearch's `saved_*`/`last_match_*` — the smallest self-contained user;
2. mark and the mark ring (`src/yank.c`), which makes `C-x C-x` and
   `C-u C-SPC` survive edits for the first time — that is a **user-visible
   behavior change**, so it needs `README.md` and a PTY case;
3. Lisp `save-excursion`, registers and diagnostics later.

Phase invariant: marker positions match a reference flat-string model for
every generated edit, and no consumer keeps a parallel `(row, col)` copy.

## Phase 6 — Compact decorations

Depends on markers being stable.

A decoration is: start marker, end marker, face category enum, priority, an
optional short id, and evaporate/stickiness flags. Not arbitrary Emacs text
properties.

The renderer queries visible decoration runs and never calls Lisp. Edits
relocate endpoints via markers and drop evaporating empty ranges.

First consumer is the isearch highlight: delete `saved_hl`, `saved_hl_line`
and the `RESTORE_HL` macro (`src/search.c:22-30`) and their four use sites, and
express the match as a decoration. That removes a real stale-index hazard, so
land it with a case that edits the buffer between snapshot and restore.

Then: compiler diagnostics (needs Plan 09's compilation handles) and a
trailing-whitespace package as the extension proof.

A sorted vector per buffer is fine at current decoration counts. Measure
before choosing a tree.

Phase invariant: no module keeps a private copy of `row->hl`.

## Phase 7 — Typed change events

```c
struct kg_change_event {
	struct kg_buffer_handle buffer;
	size_t begin;
	size_t old_len;
	size_t new_len;
	uint64_t generation;
};
```

Queued after commit; adjacent changes within one group coalesce into one event
where semantics allow. Later event types: buffer opened/killed, before/after
save, mode changed, process output/exit.

Bound queue length and memory. On overflow, collapse to a single "buffer
changed broadly" event for that buffer rather than dropping events that carry
ownership information.

Also in this phase: retire the two `dirty != dirty_before` change detectors in
`src/kbd.c` in favour of `content_generation`.

Phase invariant: every committed transaction produces exactly one event or one
coalesced group event, and the queue cannot grow without bound.

## Phase 8 — C hooks, then deferred runtime hooks

- C hooks run only at documented safe points.
- Runtime (Fe) hooks are queued and drained at the top of the command loop
  (`src/main.c`'s loop, next to `compilation_poll()`), never from the
  renderer, a signal handler, the raw input decoder, a partial edit, or
  minibuffer internals.
- Callbacks carry handles and re-resolve them; the buffer may have been killed
  between queue and drain.
- Each callback runs under the existing Fe step budget and `C-g` check
  (`kg_lisp_set_interrupt_check`).
- One failing callback must not corrupt queue ownership.

Start with after-change, buffer-opened/killed, before/after-save,
mode-changed. `pre-command`/`post-command` wait for Plan 11's command registry.

Phase invariant: with `WITH_LISP=0` the queue still exists and C hooks still
fire; with `WITH_LISP=1` no Fe call happens outside the drain point. Assert the
second with a re-entrancy counter, not by inspection.

## Phase 9 — Migrate all mutation callers

Batches, in order:

1. ordinary insert / delete / newline (`buffer.c`, 15 sites);
2. kill and yank (`yank.c`, `kbd.c`);
3. search and replace (`search.c`);
4. word, paragraph, case, join, sort (`word.c`, 13 sites);
5. rectangle (`rect.c`);
6. file insert and revert (`fileio.c`, `bufmgr.c`);
7. shell, compilation, special buffers (`shell.c`, `compile.c`, `bufmgr.c`);
8. Lisp natives (`lisp.c`).

Per batch: add exact before/after tests; migrate; delete the corresponding raw
helper export and `undo_push` call; compare undo granularity against the PTY
cases; tighten the gateway checker's deny-list.

`suppress_undo` is deleted with the last caller, not before. Its three
meanings map to: `KG_EDIT_NO_UNDO` (batches 6-7), `KG_EDIT_REPLAY` (undo.c),
and edit groups (batches 3-5).

Phase invariant after each batch: the batch's commands route through the
transaction, and the previous batches still do.

## Model and property testing

Reference model: a flat `char *` plus a marker list.

- random and exhaustive small replacements compared against the model;
- allocation failure injected at every preparation point;
- undo restores exact bytes and exact clean state;
- marker positions match the reference for both gravities, including
  multi-marker ordering and markers at range endpoints;
- UTF-8 positions that were glyph boundaries stay glyph boundaries;
- `dirty` equals "differs from the saved snapshot";
- `content_generation` advances only on commit;
- one event per group; callbacks never observe partial state.

Fuzz transaction misuse (commit twice, abort after commit, nested begin) and
require API errors rather than corruption. Extend `test/fuzz_keypress.c` rather
than adding a new fuzzer if the existing harness can reach the paths.

## Not in this plan

- No redo (add `new_text` to `undo_change` when redo is actually specified).
- No narrowing, no overlays beyond the fixed decoration record, no text
  properties.
- No screen damage tracking; `editor_refresh_screen()` keeps redrawing fully.
- No new Lisp API surface; only the adapter keeps working (Plan 12).
- No undo tail/deque rewrite — that is Plan 08 phase 7 and can land in
  parallel.

## Acceptance

```sh
make check
make WITH_LISP=0 clean all check
make fuzz-smoke		# fuzz-keypress-smoke is the load-bearing one here
.ci/run-ci-steps.sh --parallel
```

Exit criteria:

- direct user-buffer row mutation outside `buffer.c` and the transaction is
  rejected by `utils/check_mutation_gateway.py` or an assert-build abort;
- `suppress_undo` is gone and `rg 'undo_push'` returns only `undo.c`;
- every compound command uses one edit group;
- mark, mark ring and isearch state are marker-backed and survive edits;
- `saved_hl` / `RESTORE_HL` are gone;
- deferred hook tests pass with Fe enabled and disabled;
- OOM injection at every preparation point leaves no partial edit.

## Landed / deferred

### Landed

| Phase | State | Commits |
| --- | --- | --- |
| 0 — characterize the change detector | complete (added; not in the plan as a phase) | `91bf0ec` |
| 1 — inventory and seal raw mutation | complete | `aa5782d` |
| 2 — flat position translation | complete | `4374afe` |
| 3 — failure-atomic replace | complete | `e5ed3da`, `fbf7701` (analyzer follow-up) |
| 7 — first item: retire the dirty change detectors | complete | `e1ad15c` (the signal), `9c05b0a` (the two readers) |
| 4, 5, 6, 7 (rest), 8, 9 | not started | — |

The transaction exists, is failure-atomic, and four callers are on it:

```sh
rg 'kg_buffer_replace' src/            # buffer.c (impl + 3), undo.c, yank.c (2)
rg 'editor_replace_rows_from_text|editor_flat_offset_to_row_col' src/   # nothing
rg 'dirty_before' src/                 # nothing
make gateway-check                     # 209 sites, was 227
```

Counters at the close: scc 4269 against the 4280 cap (4265 at the start,
so the whole layer cost 4 points net, the migrations having paid for it);
`make check` 273/273 both configurations (237 + 36 skips with
`WITH_LISP=0`); the mutation census down from 227 sites to 209.

### Deviations, all deliberate

- **The 45 `undo_push` sites in the plan's inventory are 42.**  Plans
  02, 03, 08 and 09 moved the count; the classification table is in
  `aa5782d`'s message, and the counts per file are the manifest.
- **Phase 1 seals with a manifest, not with `static`.**  Not one row
  primitive can be made private yet -- `editor_insert_row` alone has
  callers in bufmgr, dired, rect, syntax, undo and word -- so the whole
  of phase 1 is the deny-list the plan allows for, plus the census of
  hand-written undo records, `suppress_undo` mentions and direct writes
  to a row's text fields.  There is no assert-build "changed outside a
  transaction" counter: with four callers migrated it would fire
  everywhere, and it wants to land with the last batch of phase 9, not
  the first.
- **`KG_EDIT_REPLAY` landed in phase 3, not phase 1**, where it has a
  user.  An unused option is not a seal.
- **Phase 2 did not reimplement `editor_offset_to_rowcol()` on the byte
  layer.**  It converts a codepoint offset to a row and column, which is
  a different unit, not a different spelling; `editor_char_offset()` did
  move its clamp onto the byte layer, which is where the two dialects
  could have come to disagree.  The rule ("bytes for the editor,
  codepoints for the Lisp adapter") is written next to the declarations
  in `def.h`.
- **Phase 3's tests are in `test/test_buffer.c`, not a new
  `test/test_edit.c`.**  The transaction lives in `buffer.c` and its
  tests need the same setup; a second binary buys nothing yet.  There is
  no allocation-failure injection harness -- the failure paths are
  covered by refusal cases (`test_edit_refusal_changes_nothing`), which
  prove the "nothing published" half but not the OOM half.  A malloc
  interposer is the missing piece and is worth its own commit.
- **The 43 hand-written `dirty` writes became `buffer_note_change()`
  before anything read the generation.**  The plan has `content_generation`
  arriving with the transaction and the kbd.c detectors moving in phase
  7; taken in that order the detectors would have gone blind to every
  path the transaction had not reached yet.  Doing the mechanical
  substitution first makes the signal complete from the moment it
  exists, and it closed a real hole: the four commands that assigned
  `dirty = 1` were invisible to the detector when the buffer was already
  modified (`test_comment_dwim_moves_generation_when_already_dirty`).
- **Undo's cursor placement for `UNDO_CHANGE` is the start of the
  restored text**, where the eleven older opcodes each have their own
  answer.  The named granularity cases are unaffected because none of
  the commands they cover is on `UNDO_CHANGE` yet.

### Deferred

**Phase 4 (edit grouping), 5 (markers), 6 (decorations), 7's queue, 8
(hooks), 9 (the remaining migration).**  Not started.  Nothing about
them changed; the plan text stands as written.

**Where the next implementer starts.**  Phase 9 batches are the cheapest
next step and they *fund* the rest: every caller moved onto
`kg_buffer_replace()` deletes a hand-rolled splice, which is where the
complexity budget for markers and decorations has to come from.  Four
callers are already across and each paid for itself; `.ci/mutation-gateway.json`
is the worklist, ordered by how much each file still does by hand
(`buffer.c` 73 sites, `word.c` 36, `bufmgr.c` 24, `undo.c` 22).  The
batch order in phase 9 still holds, with one amendment: batch 1 is
partly done (`editor_row_replace_range`, `editor_insert_text_raw_bulk`,
`editor_delete_region_range`, `editor_delete_text_range_raw`,
`editor_transpose_chars`), and what is left of it is the split/join
family -- `editor_insert_newline{,_raw}`, `editor_del_char`,
`editor_del_forward_char`, `editor_kill_line` at end of line -- which
share one shape: a single separator byte appearing or disappearing.

Phases 5 and 6 depend on nothing that is missing: markers hook into
`edit_publish()`, which is the single point where every published edit
now passes.  `RESTORE_HL` (`src/search.c:22-30`) is untouched and is
still decoration consumer #1.

One environmental note: `.ci/ci-06-static-analysis.sh` reported
`src/compile.c:389` (ArrayBound on the pending-line buffer) and did so
before this plan started -- verified on a clean worktree of `2edfac6`,
where it is the only finding.  It is not plan 10's; oversight pass 5
fixed it, along with the reason nothing but a hand-run of that script
ever saw it (`run-ci-steps.sh` exported `PARALLEL`, which GNU parallel
reads as its own default options, so both of ci-06's analyser phases ran
no job and exited 0 under the runner).

Also from that pass, against this plan's work: `edit_publish()` did not
number the rows it installed when the row count was unchanged (`C-t` at
the start of a line, fixed); `editor_query_replace()` resumed on a row
the replacement had just split, which AddressSanitizer reports as a
heap-buffer-overflow (fixed, with two Emacs-oracle cases); and the
staging comment's atomicity claim now says text rather than everything,
because the commit's `editor_update_row()` pass still allocates.  Three
more findings against phases 3 and 7 are open and listed in
`PROGRESS.md`.
