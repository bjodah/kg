# kg follow-up implementation roadmap

Status: authoritative follow-up plan for `stricter-emacs-adherence`,
written 2026-07-31 against the commit that landed this directory.

This series supersedes the completed 2026-07-30 review campaign and its
progress reports.  Git history retains that material.  Current feature and
technical-debt inventory remains in `doc/TODO.md`; this directory contains
only the next implementation program.

## Verified baseline

- Submodule pins are branches, per `doc/fe-upstream.md`: `fe` tracks
  `analyzers-etc` and `fe/tiny-regex-c` tracks `adapt-to-fe`.  Commit SHAs are
  deliberately not repeated here; `git submodule status` is the record.
- Clean worktree when this plan was written.
- `make check`: 20 native suites and 285 PTY cases, zero failures.
- Both `WITH_LISP` configurations and the 12-stage runner are green.
- `SCC_COMPLEXITY_MAX` is 4223 and measured usage is 4223: every additive
  commit must first delete, extract, or migrate enough complexity to pay for
  itself.
- `kg_buffer_replace()` is failure-atomic for text and has six callers (bulk
  insert, ranged row replace, transpose-chars, region delete, raw range
  delete, and `UNDO_CHANGE` replay), but
  `.ci/mutation-gateway.json` still records 209 raw mutation opinions.
- Commands have one policy table and one invocation route, but built-in keys
  still call handlers directly and `readonly_blocked_keys[]` remains a second
  read-only verdict.
- Buffer handles exist; windows still retain raw buffer slot indices.
- Visual-line mode is off by default but demonstrably pathological: the
  checked benchmark measured about 583,000 rows and 30 MB scanned per repaint
  on a 100k-line buffer, 6.8 s versus 0.18 s for the comparable session with
  wrapping disabled.

Re-read the named code before implementing.  Symbols are authoritative; line
numbers are not.

## Delivered so far

The list above is the program's starting point and is kept as the record of
it; several of its bullets are no longer true.  As of the wave-1 merge:

- `make check`: 24 native suites and 312 PTY cases, zero failures, zero
  skips.  All 12 stages green in both `WITH_LISP` configurations.
- `SCC_COMPLEXITY_MAX` is 4144 and measured usage is 4144, having gone
  4223 → 4116 (Plan 01 phases 0–5) → 4152 (phase 6's describe commands,
  the one deliberate raise, decided by the maintainer) → 4144.
- `.ci/mutation-gateway.json` records 115 raw mutation opinions in nine
  files, from 209 in fourteen.  `search.c`, `word.c`, `yank.c`, `cmd.c`
  and `rect.c` no longer mutate rows directly at all.
- `readonly_blocked_keys[]` is gone; built-in keys resolve to command
  names through the keymap.
- Windows name their buffer by a 64-bit generation-checked handle, not a
  slot index.

## Decision — the complexity cap has headroom, bounded by 4223

Taken 2026-08-01, entering wave 2.  Wave 1 ended with the cap equal to
measured usage, which is the right resting state but the wrong starting
state: it made every additive slice wait on a concurrent deletion slice,
and Plan 03's marker module, Plan 07's width cache and Plan 06's runtime
seams are all additive by construction.  `SCC_COMPLEXITY_MAX` is
therefore 4200, granting 56 of headroom.

The bound is not arbitrary and does not move: **4223 is what this program
started from**, so a follow-up program whose whole point is to remove
structural debt may not end above it.  Rule 6 is unchanged for routine
work — a slice still funds itself where it can, still records scc
before and after, and still banks a decrease when a durable saving
lands.  Headroom is for the enabling modules, not for skipping the
extraction that would otherwise pay for them.

Wave 1's residue, for whoever picks up next: `src/word.c` line coverage
sits one covered line above its floor, so the next commit touching it
should expect to add a test.  Plan 02 Phase 5 still owns
`UNDO_SPLIT_LINE`, `UNDO_JOIN_LINE`, `UNDO_REFLOW_PARA`,
`UNDO_RECT_OVERWRITE` and `editor_set_local_readonly`, which have no
producer in `src/` but are held live by tests that push records by hand.

## Review of the consultant's recommendations

The recommendations identify the right work, with these ordering corrections:

1. **Command identity plus normalized keys and layered keymaps remains first.**
   Treat command identity and the keymap migration as one workstream.  A hidden
   prerequisite is that most built-in key actions do not yet have command
   descriptors; add a named wrapper as each switch branch is removed.  Delete
   `readonly_blocked_keys[]` only after every editing key, including newline,
   delete, yank, and self-insert, reaches the authoritative policy.
2. **Finish the edit gateway before adopting markers.**  Marker storage and
   relocation tests may be written earlier, but isearch, marks, yank spans, or
   registers cannot use markers while ordinary edits still bypass
   `edit_publish()`.  Otherwise the first legacy edit makes the marker stale.
   Fix failed `UNDO_CHANGE` replay before widening gateway use.
3. **Split Plan 09's remaining work.**  Window handles and lifecycle invariants
   are useful correctness work.  Lifecycle notifications belong in the typed
   event queue, not a parallel callback mechanism.  Session nesting is a
   non-enabling rename and should follow the keymap state rewrite and kill-ring
   redesign so those fields move only once.
4. **Deliver Plan 13 by dependency-ready slices.**  The first product train is
   command introspection, kill ring/yank-pop, transpose-words, navigable
   compilation, then registers.  Do not start line numbers before visual-line
   geometry has one width model, or process-backed packages before the event
   and process services exist.
5. **Runtime work starts after the event queue, except for its Fe call seam.**
   `FeCallWithOptions` and internal `lisp.c` decomposition can proceed earlier.
   Editor callbacks, hooks, process filters, and packages wait for safe-point
   delivery and stable object handles.
6. **Promote visual-line indexing ahead of optional runtime packages.**  The
   performance gate is already met.  Start with per-row width caching and a
   persistent view-owned prefix vector.  A Fenwick tree is conditional on the
   edit-and-repaint benchmark, not the default design.

## Plan index

| Plan | Outcome | May start |
| --- | --- | --- |
| [01](01-command-identity-and-keymaps.md) | Stable command identity, normalized key events, layered keymaps, generated introspection | **Phases 0–6 done**; the decoder flag day remains, see its Status section |
| [02](02-edit-gateway-completion.md) | Replay safety, explicit internal-edit policy, all observable mutations through one gateway | **Phases 0–3 done**; Phases 4–5 are the next slice, see its Status section |
| [03](03-markers-decorations-and-events.md) | Stable markers, compact decorations, bounded typed events and C safe points | Marker core during 02; consumer conversion after 02 |
| [04](04-window-handles-and-session-lifecycle.md) | Window buffer handles, lifecycle invariants/events, later session nesting | **Phases 0–2 done**; Phase 3 blocked on 03's event queue, Phase 4 deferred |
| [05](05-emacs-affordances-delivery.md) | Dependency-ready Emacs habits without new one-off dispatch | Per bundle |
| [06](06-runtime-and-lisp-extensibility.md) | Bounded direct Fe calls, editor objects, hooks, processes, proof packages | Preparation now; callbacks after 03 |
| [07](07-visual-line-geometry-index.md) | Warm repaint independent of total buffer bytes; bounded prefix lookup | After a funding/extraction commit; coordinate with 03 display work |

## Dependency and delivery order

```text
01 command identity + keymaps ───────────┬──────────────> 05 affordances
                                         │
02 complete live edit gateway ─> 03 markers/decorations/events ─┬─> 05
                                         │                      └─> 06 runtime
04 window handles ───────────────────────┘

07 visual-line cache/index is independent after complexity is funded.
04 session nesting waits for 01 and the kill-ring slice of 05.
```

Recommended waves:

1. In parallel: Plan 01 through the global keymap; Plan 02 through ordinary
   live-buffer migrations; Plan 04 window-handle correctness.  Start Plan 07
   if one of those tracks has freed enough complexity.
2. Finish Plan 02's observable rebuild paths.  Land Plan 03 markers, convert
   consumers, then decorations and the event queue.  Finish Plan 01's mode
   layers and introspection.
3. Deliver Plan 05's kill ring/yank-pop, transpose-words, compilation
   navigation, and registers as their exact prerequisites turn green.
4. Land Plan 06's safe callback/object/process slices and proof packages.
   Perform Plan 04 session nesting only after the state shapes are stable.

## Rules for every implementation slice

1. One semantic change per commit; characterization precedes changed behavior.
2. A module gets a self-contained header.  Do not add module-owned declarations
   back to `def.h`.
3. Commands, key bindings, edits, lifecycle events, and process ownership each
   use their single registry/gateway; no temporary second policy tables.
4. Preserve `WITH_LISP=0`.  Core headers and modules never gain Fe types.
5. Keep memory/work bounded and report truncation or exhaustion distinctly.
6. No complexity or coverage ratchet is raised for routine work.  Record scc
   before/after and lower a ratchet when a durable saving lands.
7. User-visible behavior or keys update `README.md`, `doc/kg.1`, and
   `src/help.c`; run `make docs-check`.
8. Interactive behavior uses focused PTY cases; pure state machines and data
   structures use native tests; performance gates use counters rather than CI
   wall time.
9. Each commit runs its focused suite.  Each plan phase ends with `make check`
   and `make WITH_LISP=0 clean all check`.  A completed workstream ends with
   `.ci/run-ci-steps.sh --parallel`.
10. Fe or tiny-regex-c changes land and pass in the submodule first, then the
    parent pin moves in a separate kg commit.

## Program completion

This follow-up program is complete when all non-conditional phases in Plans
01–07 are green, every dependency-ready Plan 05 bundle named for the first
product train has landed, and the remaining conditional work has fresh
measurements and an explicit keep/defer decision.  Update this index as phases
land; do not create another progress report beside it.
