# Implementation progress ledger

Updated 2026-07-31, at the close of the second oversight review pass.
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
| 09 session/buffer/view ownership | next in sequence |
| 10 transactions/markers/hooks | Plan 09 phases 2–4 |
| 11 command/keymap/mode registries | phases 1/2/6/7 anytime; modes after 09 |
| 12 runtime/process/Lisp extensibility | 09/10/11 foundations |
| 13 Emacs affordances | per-bundle dependencies (kill ring needs 11 phase 3) |
| 14 coordinate-space invariants | after 03 (met); `RESTORE_HL` interim fix is its phase 2 |
| 15 structural/toolchain hygiene | anytime; phase 4 partly done by 06/07 (CBMC repaired, drivers honest) |

## Deferred items worth remembering

- Visual-line repaint cost: gate met (583k rows / 30 MB scanned per
  repaint on a 100k-line buffer; 6.7 s vs 0.18 s) — first performance
  candidate when complexity budget allows (Plan 08 phase 8).
- `transpose-chars` still flattens the whole buffer (same shape Plan 08
  phase 6 removed for insert).
- Backward regex search remains O(n²) per row; needs either an anchored
  engine entry point or a policy decision (kg's backward selection rule
  is deliberately not Emacs' bounded search).
- `buf_save_all` conflict guard landed in oversight pass 1; the remaining
  save-path gap list is in Plan 04's deferred notes.
- fe unwind/cleanup design exists (`fe/doc/unwind-design.md`); no code.
- Complexity budget: kg scc is within ~12 points of its cap again — the
  next structural plan should bank extractions early (Plan 15 phase 1 material).
