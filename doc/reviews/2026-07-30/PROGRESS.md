# Implementation progress ledger

Updated 2026-07-31, at the close of Plan 10 phases 1-3.
Source plans: [plans/00-master-roadmap.md](plans/00-master-roadmap.md).
Each plan doc now carries its own deferred-work section where phases were
consciously skipped; this file is the cross-plan index.

## Landed

| Plan | State | Highlights |
| --- | --- | --- |
| P0 | complete | 8 buffer/window characterization PTY cases (goal-column leak pinned as XFAIL); truthful hosted `make check` (tool discovery, `--require-tools`, `PYTHON` selection); the one sanctioned ratchet re-baseline (`editor_process_keypress` 120→85, O0/analyzer workaround removed); fresh baselines (real coverage 76.1%/89.3% over all 28 files); fuzz artifact hygiene |
| 01 | complete | `display_glyph_at()` unifies escaping and width in `width.c`; renderer-wide control-byte escaping (buffer, mode line, echo, Dired, picker); statusmsg emphasis replaces raw CSI; `tty_write` through `write_all`; cursor-report parser + geometry clamps; `ascii_is_*` + `ci-11` signedness lane; one-key unread slot for malformed UTF-8; `BYTE=` harness token |
| 02 | complete | glyph-granular Backspace/Delete with full-span undo; EOL `C-k` records `"\n"`; clean-checkpoint invalidation on eviction + shared `undo_stack_init`; `editor_insert_row` exact-length copy; explicit prefix zero (`cx_prefix_arg` deleted) |
| 03 | complete | `kg_utf8_forward_boundary`/`kg_regex_next_offset`; zero-width replace-all terminates (was an infinite loop on UTF-8); Emacs-verified stop-at-region-end; `KG_REGEX_TOO_COMPLEX` surfaced; isearch matches buffer bytes and puts point in chars space; `editor_row_replace_range` one-shot replacement |
| 04 | complete | one compilation retained-byte budget (newline and unterminated-line bypasses closed); `file_snapshot`/`file_identity` three-state disk change detection; write-file destination-exists prompt before name adoption; pre-rename recheck + dir fsync; `file_read_all` checked reads; Dired collect/verify deletion over a dirfd; pre-existing filename use-after-free fixed |
| 05 | complete (phases 1–8; 9 designed, 10 deferred) | fe: bounded cycle-safe writer (`M-:` on a cyclic list no longer hangs kg); dotted-list reader rejects malformed forms; macro expansion no longer rewrites call sites; binder `&optional`/`&rest` + opt-in strict arity; unbound sentinel with `boundp`/`makunbound`/void-variable; Fex file ownership + finalizer; exact-byte string copies. Also surfaced: fe's `assert-equals` family had been a no-op suite-wide |
| 06 | complete (phases 1–5, 8 seeds; 6–7 deferred) | tiny-regex-c: published alignment contract with refusal; single emitter (fixed silent prefix-truncation); explicit parse stack with an Emacs-verified acceptance table; per-execution budgets + `re_exec_with_options` (cancel callback); honest `TOO_COMPLEX` on the repeat ceiling; submodule sanitizer CI had never been armed — fixed |
| 07 | complete (stateful fuzz targets, mutation, portability deferred with notes) | `ci-12-subprojects` runs both submodule suites from root; per-symbol pmccabe baselines (kg 657 symbols, fe 189; new functions ≤15); per-file coverage floors with measured jitter allowance; `quality.json` + per-case results both layers; fuzz time budgets + tracked seeds (first seeded run found and fixed a `.dir-locals.el` NUL editor hang); differential walks whole match successions; hosted one-job-per-step workflow |
| 08 | complete (phases 1–6; 5b/7/8/9/10/11 measured and deferred) | 22 perf counters + `test_perf` gate + `make bench`; 1M-line load 3341→756 ms (single highlight pass, syntax selection hoisted off the per-row path); frame buffer and row capacities (frames verified byte-identical); rect one-shot column edits (`KG_EDIT_NO_UNDO`); multiline insert local splice (no whole-buffer flatten) |
| 09 | complete (phases 0–5; 6 and 7 deferred with notes) | one owner per field: buffers own text/undo/syntax/dirty, file identity, local options and marks; windows own point, scroll and goal column. All six copy protocols deleted (`buf_save_to_slot`, `buf_restore_from_slot`, `buf_save_current_state`, `win_save_active_view`, `win_restore_active_view`/`win_activate_window`, `buf_temp_swap_in/out`) plus the global `undostack` and `editor_set_syntax`'s dual write; `(id, generation)` buffer handles retire compilation's slot indices and kbd.c's filename-pointer identity; the goal-column leak across `C-x b` is fixed (P0's XFAIL flipped); `test/test_winmgr.c` is the first native model of buffers and windows together |
| 10 | partial (phases 1–3 and phase 7's detectors; 4–6, 8, 9 not started) | one mutation gateway exists and is failure-atomic: `kg_buffer_replace()` stages the replaced text, the rows that replace it, the row-array growth and the undo record, and only then publishes, so a refused or failed edit leaves text, undo, modified flag and generation untouched. `UNDO_CHANGE` is one record for any edit and replays through the same primitive (`KG_EDIT_REPLAY`), so there is no second splice to disagree. Four callers migrated, each of which *was* a second splice — transpose-chars (no longer rebuilds every row), multiline insert, `editor_row_replace_range`, yank's region delete — paying for the layer: scc 4265 → 4269. Byte positions are the editor's one position dialect (`buffer_byte_length`/`row_col_to_position`/`position_to_row_col`), codepoints are the Lisp adapter's alone. `content_generation` replaces the dirty counter as the "did this command edit?" signal, closing the hole where `dirty = 1` said nothing on an already-modified buffer. `make gateway-check` is the census of what still mutates by hand: 227 → 209 sites, may only shrink |

Oversight cadence: intermediate Opus review after every two steps
(4 passes, 13 follow-up commits across the three repos), plus two direct
review passes by the coordinating agent (dead-code deletion restoring ~50
scc points; `C-x s` changed-on-disk guard with stale-snapshot fix; this
ledger).

## Current pins

kg → fe `analyzers-etc` → tiny-regex-c `adapt-to-fe`; all three suites and
the 12-step kg runner green at every landed commit. See `git submodule
status --recursive` for SHAs.

## Not yet started

| Plan | Waiting on |
| --- | --- |
| 11 command/keymap/mode registries | unblocked (ownership landed) |
| 12 runtime/process/Lisp extensibility | 09/10/11 foundations (10's hook queue is its phase 8, not started) |
| 13 Emacs affordances | per-bundle dependencies (kill ring needs 11 phase 3) |
| 14 coordinate-space invariants | after 03 (met); `RESTORE_HL` interim fix is its phase 2 |
| 15 structural/toolchain hygiene | anytime; phase 4 partly done by 06/07 (CBMC repaired, drivers honest) |

## Deferred items worth remembering

- Visual-line repaint cost: gate met (583k rows / 30 MB scanned per
  repaint on a 100k-line buffer; 6.7 s vs 0.18 s) — first performance
  candidate when complexity budget allows (Plan 08 phase 8).
- `transpose-chars` no longer rebuilds every row (Plan 10 phase 3 routed
  it through the edit transaction).  It still serialises the buffer once
  to find the two glyphs around point; doing that on the rows would need
  a glyph walk across the row separator, which is marker-shaped work.
- Backward regex search remains O(n²) per row; needs either an anchored
  engine entry point or a policy decision (kg's backward selection rule
  is deliberately not Emacs' bounded search).
- `buf_save_all` conflict guard landed in oversight pass 1; the remaining
  save-path gap list is in Plan 04's deferred notes.
- fe unwind/cleanup design exists (`fe/doc/unwind-design.md`); no code.
- Complexity budget: kg scc is at 4276 against a cap of 4280 — four
  points of headroom, the tightest it has been.  Plan 10's transaction
  cost 4 points net because each caller it took over deleted a
  hand-rolled splice, and oversight pass 5's four defect fixes cost 7
  more; the remaining phases (markers, decorations, hooks) have to be
  funded from phase 9's migration, and the next commit that is not a
  migration should expect to have to find its own room first.
- `src/compile.c:389` (ArrayBound on the pending-line buffer) is fixed
  (oversight pass 5).  It was the only finding in the whole compilation
  database once ci-06's analysers were made to run at all — see the
  `PARALLEL` note below.
- Plan 09 phase 6 (nest the session record as `struct kg_session`) and
  phase 7 (lifecycle hooks, `winlist[].bufidx` as a handle) are not
  started; the plan doc's "Landed / deferred" section says where to pick
  up.  Nothing depends on either.
- Plan 09 phase 3 moved the file-identity and option fields flat rather
  than grouping them into `struct file_snapshot` / `struct
  buffer_options`; the grouping is now an independent rename.
- `run-ci-steps.sh` exported `PARALLEL`, which is GNU parallel's own
  variable for default options, so ci-06's clang-check and clang-tidy
  phases ran no job and exited 0 under **both** runner modes; only
  running `.ci/ci-06-static-analysis.sh` by hand ever analysed anything.
  Fixed in oversight pass 5, along with two guards that make the same
  class of silence loud.  Worth remembering as a shape: a step that
  passes suspiciously fast is a step to read, and any gate whose "no
  findings" answer is the absence of output needs a positive count.

## Open, from oversight pass 5

Found by audit, judged real, and **not** fixed — with what is known:

- `editor_undo()` pops an `UNDO_CHANGE` record, replays it through
  `kg_buffer_replace()` without looking at the return value, and frees
  the record either way (`src/undo.c:270-272, 298, 433-441`).  A refused
  replay therefore loses the record, and with one edit outstanding the
  pop alone makes `size == clean_size`, clearing the modified flag on a
  buffer that still differs from disk.  Only reachable through an
  allocation failure today; it stops being only that as soon as a
  `KG_EDIT_NO_UNDO` caller can shrink a buffer with `UNDO_CHANGE`
  records under it, which is phase 9 batches 6-7.  Fix is local: re-link
  the record and return on failure.
- `edit_publish()` frees the head row (`editor_free_row()`) and installs
  a fresh `erow`, so every gateway edit throws away that row's `render`
  and `hl` allocations and their capacities — the thing
  `editor_row_grown_capacity()` exists to preserve.  One backspace in a
  1 MiB row now costs large frees and mallocs where it cost a `memmove`.
  Measure `KG_PERF_RENDER_ALLOC` per keystroke before phase 9 widens the
  set of callers on this path.
- `editor_string_rectangle()` (`src/rect.c:443-452`) passes prompt text
  straight to `editor_row_replace_range()` per row.  `C-q C-j` in that
  prompt now splits rows, so the loop walks into rows it just made and
  the coarse `UNDO_RECT_OVERWRITE` cannot restore the result.  Before the
  transaction the separator was embedded in the row instead — wrong in
  memory, right on disk.  Either refuse a separator there or teach the
  loop the shape, the way `editor_query_replace()` now knows it.
- The shift-select teardown in `key_finish_keypress()`
  (`src/kbd.c:669-678`) writes through `bcur()` without the
  `buffer_before` identity guard its neighbour has.  Not reachable
  through `C-x b`, which returns from `handle_cx_prefix_key()` before
  the teardown runs — so it is an asymmetry, not a demonstrated defect,
  and closing it wants a command that both switches buffers and goes
  through the ordinary dispatch.
- `win_delete_others()` (`C-x 1`, `src/winmgr.c:277-288`) deactivates
  the other windows without `buf_remember_view()`; it is the one detach
  path that does not bank the view it is dropping.
- Auto-revert still only reloads the current buffer
  (`src/bufmgr.c:327-333`) on a rationale the ownership work retired:
  points are in `winlist[]` now and are all enumerable.
- The mutation census is textual and gameable in more ways than its
  docstring admits: wrapping N raw calls in one helper banks a
  "decrease", `r->size` hides from a probe that hard-codes the
  identifier `row`, `memcpy(row->chars, ...)` matches nothing, the glob
  is non-recursive and `.c`-only, and `dirty` is not a probe at all.
  Cheap to tighten; the helper-wrapping hole needs a rule, not a regex.
- The mode line of a *non-selected* window is correct by construction and
  has no PTY case asserting it.
