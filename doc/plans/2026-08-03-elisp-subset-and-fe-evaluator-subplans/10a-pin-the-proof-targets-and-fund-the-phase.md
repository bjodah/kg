# Sub-plan 10A — Pin the proof targets, correct the parent, fund the phase

Phase 10 of `doc/plans/2026-08-03-elisp-subset-and-fe-evaluator.md` (§14
"Compatibility proofs", §15 "Bytecode and replacement decision gate"). First
of the tenth set; no prerequisites. Like every A-slice it changes no
behaviour: it records what is true, decides what each proof means, and funds
the work. Every number below was measured 2026-08-07 at kg `94a931c` /
fe `56c60f9` by the Phase 10 fact audit; per Rule 6 the implementing slices
re-measure at slice start. (The Phase 9 fix cycle moved both HEADs after the
audit; the A-slice re-measures the caps table first.)

## Why §14 is stale

1. **Proof 1's file is `lisp/auto-fill.el`, and has been since Phase 2.**
   §14:1599 says "Migrate `lisp/auto-fill.fe` to the new dialect and
   eventually to `.el`" — there are zero `.fe` files tracked in kg. Of the
   seven bullets, six are already exercised by the file and its three passing
   PTY cases (functions/lexical variables, hooks, buffer positions,
   `save-excursion` — the `doc/lisp-api.md` worked example — implicit strict
   arity, `provide`/`require`). The only genuinely absent bullet is
   *conditions*: the file contains no `condition-case`/`unwind-protect`.
   What §14 does not list is what is actually wrong: the PTY cases plant an
   inline copy whose `auto-fill-mode` docstring has drifted from the tracked
   file while `lisp/auto-fill.el:22-27` claims byte-for-byte identity, and
   `make install` ships no `lisp/` at all while `README.md` and `doc/kg.1`
   tell users to `(require 'auto-fill)`.
2. **Proof 2's fixture exists as a distributed corpus.** ~97 PTY cases plant
   `config_files:` HOMEs (~70 with `.config/kg/init.el`);
   `test/pty/lisp-init-phase8-library.yaml` is 08A's representative init
   reconstructed construct for construct (40 top-level forms), and
   `test/test_perf.c` and `utils/bench.py` carry the same text as in-C
   fixtures. Coverage against §14's bullets: seven of eight, with
   *buffer-local-style configuration* nominal only — `setq-local` and
   `setq-default` are literal aliases of `setq` (`lisp/prelude.el:552-553`);
   there is no buffer-local variable namespace, though `add-hook`'s LOCAL
   argument is real. Error handling exists in separate cases, not in the
   fixture.
3. **Proof 3 has no artifact, and its "macro expansion" bullet is ambiguous
   at a price cliff.** Everything else it lists already works and agrees
   with the pinned Emacs (measured: Lisp-2 separation, `funcall`/`apply`,
   closures, catch/throw, condition-case, provide/require, load chaining —
   24 of 30 pure-language snippets byte-agree). But `macroexpand`,
   `macroexpand-1` and `macroexpand-all` do not exist anywhere — not as
   natives, not in the prelude (impossible there: fe's `funcall`/`apply`
   reject a macro operand by design), not in any manifest, TODO or doc. If
   the bullet means the reflective functions, it is fe C work; if it means
   "the package uses macros", it is free. §14 does not say which.
4. **The milestone gate's oracle item is asserted, not assertable, on the kg
   side.** fe's half is automated (`fe/utils/run-fe-compat.py`: 306 cases,
   0 failed). kg's 99 `comparison: emacs` cases have no runner —
   `test/lisp-compat/README.md` says a human reads case and snapshot side by
   side. `test/kgbatch` (built by the Phase 8 audit) closes the gap but
   needs a `prin1`-shaped print (5 string-valued cases) and a live scratch
   buffer (2 buffer-touching cases die with `current buffer is dead`). The
   audit's ad-hoc run: 82 of the supported cases byte-agree; one entry is
   misclassified and can never pass (`native-string-length`:
   `supported`/`comparison: emacs` against a `void-function` snapshot —
   Emacs 31 has no `string-length`).
5. **Two silent divergences are unrecorded — the sharpest §2 violations
   measured.** (a) `defvar` does not create a special variable:
   `(progn (defvar v 1) (defun f () v) (let ((v 2)) (f)))` answers 1 where
   Emacs answers 2, silently; `prelude-defvar` sits in the manifest as
   `supported`/`comparison: emacs` with an empty rationale. (b) The writer
   never abbreviates `(quote x)` → `'x` or `(quasiquote …)` → backquote —
   visible in every `M-:` echo and `%S`; recorded only inside another
   entry's rationale. Two `divergent` entries also have cases that fully
   agree with the oracle — the recorded divergence is untested
   (`native-type-of`: case never evaluates `(type-of 1.0)` → `double`;
   `native-commandp`: the anonymous-lambda answer untested).
6. **§15 is partly answerable today, from counters that exist.** Prelude
   load is instrumented (`KG_PERF_LISP_PRELUDE_NS`): 0.8 ms, with zero
   collections at every measured workload, and a full representative init
   leaves the arena 12.0% live (peak_live 6729 / 56224). Triggers 1-3 of
   §15's bytecode justification already read *no*. Six of §15's nine
   measurements have no instrumentation, and three of those six (read-vs-eval
   split, dispatch cost, GC time) are fe work.

## Decisions

1. **Phase 10 is the three proofs plus the gate machinery that makes the
   milestone assertable — not new language surface.** The one exception is
   Decision 2.
2. **`macroexpand` and `macroexpand-1` become fe primitives; `macroexpand-all`
   is rejected by name.** §14's Proof 3 bullet is read as the reflective
   functions: a "higher-order package" that cannot ask what its own macros
   expand to would prove macro *use*, which Proof 2 already proves. Price:
   fe C at zero headroom — funded per Decision 8. `macroexpand-all` needs a
   code walker; it lands in the unsupported-by-name channel
   (`void-function` is NOT acceptable — see Decision 5) and in
   `doc/TODO.md`.
3. **The kg oracle runner is gate infrastructure, not optional tooling.**
   `test/kgbatch` grows a `prin1` print mode and a scratch buffer; a runner
   script (utils/, Makefile target, CI step membership decided in-slice)
   compares every kg `comparison: emacs` case against its snapshot, with the
   same skip-with-reason discipline as the PTY harness. The misclassified
   `native-string-length` entry is re-classified truthfully.
4. **The two unrecorded divergences are recorded, not fixed.** Dynamic
   binding for `defvar` is a language-runtime project, not a proof; it gets
   a `divergent` manifest row with a case that pins the measured answer, a
   `doc/fe-upstream.md` row, a `doc/TODO.md` entry, and honest prose in
   `doc/lisp-api.md` (the current one-liner does not say the consequence is
   a silently different answer). Same recording treatment for the
   quote-printing gap. The `native-type-of`/`native-commandp` case gaps are
   closed with cases that exercise the recorded divergence.
5. **"Unsupported entries fail clearly" is met for reader syntax and not for
   functions; Phase 10 does not build a missing-function channel.** Measured:
   all three `unsupported` manifest entries produce plain `void-function` —
   byte-identical to a typo. A curated "known-name" channel would be new
   language machinery with an unbounded name list; instead the gate item is
   re-worded against what the tree does (reader syntax rejects by name;
   functions answer `void-function`), the three entries' rationales say so,
   and the debt is recorded. This is a parent correction, not a silent
   re-interpretation: recorded in the README correction block.
6. **Proof 2's fixture is the distributed corpus, declared.** No new
   monolithic fixture: `lisp-init-phase8-library.yaml` is named the
   representative fixture, error handling is added to it (the one §14 bullet
   it lacks), and the proof documentation cites the corpus. The
   buffer-local bullet is recorded as nominal (aliases) with the manifest
   row re-classified from `supported` to `divergent` if its current
   classification misleads (decide in-slice against the row's actual text).
7. **The `require`/`load` suffix asymmetry is fixed in `src/lisp_require.c`.**
   `(require 'f "name.el")` fails (`candidate_readable()` builds
   `name.el.el`) where `(load "name.el")` works — two loaders in one tree
   disagreeing about the same input, unrecorded anywhere. One conditional,
   funded per Decision 8. `load` not searching load-path stays: it is a
   recorded divergence with a manifest row.
8. **Funding (bases re-measured at the Phase 9 close, 2026-08-07).**
   kg scc **5800/5800**, fe scc **765/765**, fe pmccabe **1072/1072**
   (343 symbols) — zero headroom everywhere; kg's scan covers `src` only,
   so test/, utils/ and lisp/ work is free. Raises: **fe 765→795 scc and
   1072→1105 pmccabe in 10B's opening commit** (macroexpand pair + compat
   cases; ~+15..30 scc band); **kg 5800→5820 scc in 10C's opening commit**
   (the `lisp_require.c` conditional plus the two §15 perf counters;
   +3..10 band). Each raise with temporary-lowering proof; the close slice
   (10D) re-sets all caps to measured actuals, the convention every phase
   since 08 has kept.
9. **§15 gets its cheap evidence and an explicit answer, not instrumentation.**
   Two counters beside `KG_PERF_LISP_PRELUDE_NS` (user-init ns, package-load
   ns) land in 10C; 10D records the measured trigger table and the
   conclusion (today: no trigger fires — prelude+init 0.8 ms, 0 collections,
   12.0% arena live). Read-vs-eval, dispatch and GC-time instrumentation is
   NOT built: it is fe work at zero headroom for a decision the existing
   counters already settle. If a future phase re-opens bytecode, it funds
   the instrumentation then.

## Corrections to the parent (binding for Phase 10)

1. §14:1599 "auto-fill.fe": the migration is done; the file is `.el`
   everywhere. Six of Proof 1's seven bullets already pass; the seventh
   (conditions) plus the drift gate and the ship decision are the work.
2. §14:1613 "Create a tracked, isolated fixture": exists distributed;
   Decision 6 declares rather than duplicates.
3. §14:1620 "buffer-local-style configuration where supported": nothing is
   supported; the bullet is satisfiable only as "recorded honestly".
4. §14:1633 "macro expansion": disambiguated by Decision 2.
5. §14:1646 oracle gate: kg's half needs the Decision 3 runner to be
   assertable at all; one manifest entry could never pass and is
   re-classified.
6. §14's "unsupported entries fail clearly": met for reader syntax only;
   re-worded per Decision 5.
7. §15's list: prelude load time already exists (0.8 ms); triggers 1-3
   measured *no* today; the gate is answered with existing + two cheap
   counters per Decision 9.

## Work

1. This document, the README grouping/sequencing/price updates, and the
   parent correction block — recorded, no behaviour change.
2. The funded raises land in the B/C slices' opening commits, not here.
3. Re-measure the caps/baseline table at the post-fix-cycle HEADs and record
   it in the README baseline paragraph.

## Gates

- No behaviour change: `make check` identical before/after.
- The README's Phase 10 rows and this document agree on every figure.

## Price

0 scc both trees (documentation only).
