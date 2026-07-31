# Implementation progress ledger

Updated 2026-07-31, at the close of Plan 12's process phases (9-10).
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
| 11 | partial (phase 1 complete; phase 6's applier and phase 7's rank filter; 2-5 and 8 not started) | command policy is one table: `struct named_cmd` in `def.h` carries `CMD_EDITS_BUFFER`, `CMD_LISP_CALLABLE` and a one-line summary, and `cmd_invoke()` is the only route into a command -- it owns the read-only refusal, the Lisp-callability verdict and the prefix argument. `allowed_commands[]` in `lisp.c` is deleted (both its error strings pinned by PTY cases), and the live gap is closed: a Lisp-defined command used to fall through `cmd_execute_named()` with no descriptor, so a read-only buffer refused it only if the body reached a self-guarding native, and then as a mid-command Lisp error. Every Lisp-defined command now counts as one that edits the buffer, documented in README and kg.1. `test_cmd.c` (the tree's second everything-but-main.c binary) asserts the table's invariants. Two dedups fund it: one two-pass rank filter behind M-x and `C-x b`, and one value applier plus one name table behind the three file-local envelope scanners, whose scanners stay separate. scc 4276 -> 4256 with `SCC_COMPLEXITY_MAX` lowered at each step and never raised |
| 10 | partial (phases 1–3 and phase 7's detectors; 4–6, 8, 9 not started) | one mutation gateway exists and is failure-atomic: `kg_buffer_replace()` stages the replaced text, the rows that replace it, the row-array growth and the undo record, and only then publishes, so a refused or failed edit leaves text, undo, modified flag and generation untouched. `UNDO_CHANGE` is one record for any edit and replays through the same primitive (`KG_EDIT_REPLAY`), so there is no second splice to disagree. Four callers migrated, each of which *was* a second splice — transpose-chars (no longer rebuilds every row), multiline insert, `editor_row_replace_range`, yank's region delete — paying for the layer: scc 4265 → 4269. Byte positions are the editor's one position dialect (`buffer_byte_length`/`row_col_to_position`/`position_to_row_col`), codepoints are the Lisp adapter's alone. `content_generation` replaces the dirty counter as the "did this command edit?" signal, closing the hole where `dirty = 1` said nothing on an already-modified buffer. `make gateway-check` is the census of what still mutates by hand: 227 → 209 sites, may only shrink |
| 14 | complete (phases 1–4) | the three coordinate spaces are written down and enforced. Phase 2's `RESTORE_HL` hazard was real and keyboard-reachable: an ASan build overreads a 2-byte allocation by 36 bytes when isearch's `C-d` handoff joins the next row into the match's row and the restore then copies the *new* `rsize` (the plan predicted a shortening row, which is the safe direction; growth is the bug, and plan 01's `hl_capacity` slack does not mask it because the source is an exact `malloc`). `struct hl_snapshot` carries the saved length, the row and the buffer generation, clamps, and refuses a row whose generation moved -- `TODO(plan-10)` marks it as the interim until decorations live on the edit transaction. `doc/coordinates.md` is the audit: 15 rows, 11 `ok`, 2 fixed by plan 03 (including the confirmation that `kg_regex_match_backward()` consumed the shared glyph-boundary helper), 1 documented divergence, 2 wrong and fixed -- `generic_keyword_scan()` filling a render-indexed run with a chars length, and `coloff` being read as a render offset by `display.c` while every command writes it as a chars offset, which closes `doc/TODO.md`'s horizontal-scroll/tab item. `render_col_to_chars()` is `visual_col_to_chars()` (it takes a display column), and `KG_ASSERT_CHARS_OFF`/`KG_ASSERT_RENDER_OFF` are armed by `.ci/ci-04`, so the assertions run against the whole PTY suite. Self-funded throughout: scc 4256 → 4239, cap lowered at each of the four commits |
| 12 | partial (phases 9-10's deduplication and process-group fix; the Fe runtime phases 1-8, 10's Lisp process API and 11 not started) | kg had two `fork`/`execl` sites, not the three the plan counted -- `shell_run_capture()` was already dead-code-deleted -- and only compilation put its child in a process group.  `src/process.h` is now the one spawn/reap/kill core: `kg_process_spawn()` takes what actually differs (command, directory, stdin fd, where stderr goes, non-blocking or not) and both callers keep their own I/O strategy.  `M-!` and `M-|` children lead their own groups, so a shell command's grandchildren die with it instead of outliving kg's knowledge of them; `test_shell` proves it with a backgrounded `sleep`, and `test_compile` proves the same end to end through `C-c C-k`, in two presses because a non-interactive shell ignores SIGINT in a background job.  Raw wait statuses stop at the module boundary (`struct kg_process_status`), which retired both `WIFEXITED` ladders.  Self-funded: scc 4238 -> 4223, cap lowered at each of the five commits, paid for by dropping `shell_run()`'s pointless CLOEXEC-dup-of-a-CLOEXEC-fd dance and by `pump_io()` filling both `pollfd` slots every round instead of carrying a count and an index -- the latter found by `gcc -fanalyzer`, which lost track of the count once the fds came from another translation unit; shell.c's two `-Wanalyzer-fd-*` suppressions went with the bookkeeping they covered for.  Not built: a process table (still one child at a time) and a cancel path for `M-!` (kg is inside `poll()` and reads no keys while a shell command runs; kg(1) says so now) |
| 15 | complete (phases 1-5) | `def.h` 1279 -> 1160 lines: `cmd.h`, `keybind.h` and `syntax.h` are the first three module headers under a written convention, and `make header-check` compiles every `src/*.h` as the first thing in its own translation unit. The extraction cost no include churn beyond the sixteen direct `syntax.h` includes IWYU wanted, because spelling rows as `struct erow *` there kept `erow` attributed to `localvars.h` -- the shape the next extraction should copy. `IWYU`/`IWYU_TOOL` were the last absolute `/opt-3` paths in any of the three Makefiles; all are `command -v` with a named failure now, and `AGENTS.md` carries the toolchain list with each tool's override variable and the step that needs it. `make docs-check` (`utils/check_help_drift.py`, 95 keys) found `M-Backspace` and `C-z` in the help table, in the editor, and in no key table in kg(1); `M-d` had only a passing mention in prose. The single-slot kill ring is no longer called something a reader will expect `M-y` to work on. tiny-regex-c: `tests/test_end_anchor.c` runs, `tests/test_print.c` (which calls a removed `re_print()`) is gone, two dead constants deleted; fe's colliding `ci-06` stages are 06/07/08 and its guide says a stage number is the execution order. Two decision records landed rather than code: plan 09's buffer-slot lifecycle policy and plan 10's "who may mutate a buffer outside a keypress", the latter with the prompt-time compilation case plan 04 never added. scc 4238 throughout -- declaration moves carry no branches |

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
| 12 runtime/process/Lisp extensibility, phases 1-8 and 11 | 09/10/11 foundations (10's hook queue is its phase 8, not started; 11 phases 3 and 8 not started), plus `FeCallWithOptions` on kg's pinned fe branch.  Phases 9-10's process work landed; see the plan's own "Landed / deferred" |
| 13 Emacs affordances | per-bundle dependencies (kill ring needs 11 phase 2/3, which are not started: there is still no `ALT_Y`, so M-y and M-t remain blocked on the keymap) |

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
- The coverage baseline was last re-measured at plan 09 (`1e26dcc`);
  `make coverage-check` has reported "12 file(s) above their floor" since,
  and plans 10, 11, 14 and 15 all left it alone rather than banking a
  number that partly reflects the documented per-file jitter.  Whoever
  re-measures next should do it as its own commit.
- `M-!` and `M-|` cannot be interrupted: kg blocks inside `pump_io()`'s
  `poll()` and reads no keys while the command runs, so `C-g` never
  arrives.  Plan 12's process-group work is the prerequisite for adding
  a cancel (a group-directed signal used to mean signalling kg itself),
  but the pump would also have to poll kg's input fd.  Documented in
  kg(1) rather than fixed.
- The three module headers plan 15 extracted (`cmd.h`, `keybind.h`,
  `syntax.h`) are the first three of a longer list; `def.h` is still 1160
  lines and `bufmgr.c`/`winmgr.c`/`undo.c` all still declare through it.
- Complexity budget: kg scc is at 4223 against a cap of 4223 — no
  headroom, which is the normal state here.  Plan 11's registries were
  sequenced dedup-first for exactly this reason: the picker filter freed
  8 points, the command descriptors spent 3 of them, and the file-local
  appliers freed 15 more.  Plan 14 did the same at a smaller scale and
  entirely inside its own subject: one isearch prompt instead of four
  branches, one highlight-snapshot function instead of two copies,
  `editor_visual_col()` instead of the cursor loop's private copy of it,
  and one `win_cells()` instead of eight `if (win_w <= 0)` guards.  The
  next commit that is not a migration should expect to have to find its
  own room first.  Plan 15 needed none: moving declarations between
  headers is free, since scc's C complexity probe matches `if ` and an
  include guard's `#ifndef` is not that.
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
  points are in `winlist[]` now and are all enumerable.  Plan 15 phase 5
  wrote this down in plan 10's design note as a *conservative*
  restriction that outlived its argument rather than a correctness
  requirement, and deliberately did not change it: widening it needs a
  decision about a point a reload puts past the end, and its own case.
- The mutation census is textual and gameable in more ways than its
  docstring admits: wrapping N raw calls in one helper banks a
  "decrease", `r->size` hides from a probe that hard-codes the
  identifier `row`, `memcpy(row->chars, ...)` matches nothing, the glob
  is non-recursive and `.c`-only, and `dirty` is not a probe at all.
  Cheap to tighten; the helper-wrapping hole needs a rule, not a regex.
- The mode line of a *non-selected* window is correct by construction and
  has no PTY case asserting it.

## Open, from plan 11

- `readonly_blocked_keys[]` (`src/kbd.c:17`) is the third statement of
  the read-only verdict and the one plan 11 phase 1 could not delete: it
  keys off keycodes, and built-in keys do not resolve to command names
  until phase 3 step 1.  It agrees with `cmdtable` today.
- Plan 11 phases 2 and 3 are the blocking pair for plan 13's kill ring,
  M-y and M-t: kg has no modifier bits, Meta combinations are separate
  enumerators in one flat enum, and there is no `ALT_Y` at all.
- Plan 11 phase 6 landed the value application, not the typed variable
  registry.  The four new settable variables the plan lists
  (`auto-revert`, `visual-line-mode`, `overwrite-mode`,
  `electric-pair-mode`) each widen what a downloaded file can change and
  were deliberately not registered.
