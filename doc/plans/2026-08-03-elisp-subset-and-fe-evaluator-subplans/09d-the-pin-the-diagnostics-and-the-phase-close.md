# Sub-plan 09D — The pin, the diagnostics surface, and the phase close (kg)

Prerequisite: 09C. kg-side; contains the phase's single pin move and closes
the phase.

## Part 1 — the pin move (first, its own commit)

- Gitlink to the post-09C fe SHA; `doc/fe-upstream.md` row with measured
  figures (versions — 09B/09C add no language surface, so
  `FE_LANGUAGE_VERSION` stays 7 unless the new condition names are deemed
  language; decide against the versioning contract in `fe.h` and record —
  `FeMinimumArenaSize()`, kg 1 MiB frames/slots, the two new pre-built
  conditions, the mark rewrite's one-line description).
- Adaptation audit, measured not hoped: kg's error rendering reads the
  condition pair — verify the new named OOM condition renders through
  `Lisp error:`/`Hook error:`/`Init file error:` paths; the strings kg
  greps for ("out of memory") may have become condition names — adapt
  tests, not fe.
- `src/lisp_core.c` comment constants re-measured at the pin.

## Part 2 — the kg diagnostics surface

- One read-only command (name it in the `cmdtable` style, e.g.
  `lisp-arena-stats` — not `CMD_EDITS_BUFFER`, `CMD_LISP_CALLABLE` per the
  table's policy) rendering `kg_lisp_arena_stats()`: slots total/free/peak,
  collections, GC-stack peak, frame peak, allocation failures. Output
  through the echo area / a help-style buffer consistent with existing
  describe-* commands; strings through the safe rendering path.
- `doc/kg.1`, `README.md`, `src/help.c` if a key is bound (default: M-x
  only, no key), `doc/lisp-api.md` for the Lisp-callable form.
- The 08 lesson binds: the manifest row for it cites a test that actually
  asserts the output.

## Part 3 — exhaustion coverage, kg side

- PTY/perfobj cases pinning 09A Table X's kg rows: mid-command exhaustion
  recovers (slots free again, next command normal); mid-hook exhaustion
  reports and the host operation completes; mid-init rooted exhaustion
  leaves the session alive and *visible* through the new diagnostics
  command (Decision 3: recorded, not rescued — the test asserts the state,
  the TODO records the reset-command debt).
- With 09B in the pin: an init/command `condition-case` catching OOM *by
  name* end-to-end in the editor.
- `test_lisp.c`: the `(error …)`-catches-OOM contract through kg's
  evaluator entry points; `kg_lisp_shutdown` after an exhausted session
  (leak gate already fails CI on leaks — extend, don't duplicate).
- A low-stack guard note: the `ulimit -s 1280` segfault from 09A
  disappears with 09C; assert the fixed behaviour with a bounded-stack
  subprocess test in the perf/native harness if cheaply expressible,
  else record it as verified-by-hand in the Status with the command.

## Part 4 — the phase close

- Re-set both trees' caps at measured actuals (fe scc/pmccabe from the
  09B raise; kg 5860 back to actual), temporary-lowering proof in the body.
- README Status — Phase 9 written by the orchestrator at acceptance (per
  the Phase 8 process correction: the implementer does not close the phase,
  does not delete the sub-plan documents, and does not write "after
  reviewer acceptance" — the reviewer does).
- `doc/TODO.md`: the deferred reset command (Decision 3), the cleanup
  registry staying fe-standalone (Decision 4), and closure of the mark
  items.
- Full parallel runner green: `JOBS=8 .ci/run-ci-steps.sh --parallel`.

## Tests owned by this slice

Enumerated above per part; the checklist in the handoff table is the
acceptance list, and (08's lesson, twice-learned) *the enumerated tests are
the slice* — an implementation without them is an unfinished slice.

## Price

kg +5..15 scc (one command, its renderer, tests). Funded by 09A Decision 7
(5804→5860 at the opening commit, re-set at close).

## Explicitly not this slice

No fe edits beyond the gitlink. No reset/GC-user command (recorded debt).
No new stat fields (the API is complete). No Budget catchability change.
