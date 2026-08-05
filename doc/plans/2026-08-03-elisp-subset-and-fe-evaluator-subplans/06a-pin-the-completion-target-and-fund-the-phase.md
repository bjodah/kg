# 06A — Pin the completion target and fund the phase

Parent: [Phase 6](../2026-08-03-elisp-subset-and-fe-evaluator.md#10-phase-6--structured-errors-and-non-local-exits).
No behaviour changes in this slice.  Its outputs are oracle snapshots,
five recorded Decisions, one funded complexity raise, corrections to the
parent's and the design document's stale claims, and an updated
`fe/doc/unwind-design.md` — the 00A/02A/04A/05A pattern, plus the
parent's own first instruction for this phase: *"read it and record
where the shipped `unwind-protect` took a narrower shape than the
design assumed; its last is to update it in place."*

## Why this slice exists

Phase 6 is the first phase of milestone 2, and milestone 1 closed with
the fe caps nearly spent: **scc 533/540, `fe_eval.c` 326/340, pmccabe
751/760** (measured 2026-08-05 after the post-close review fixes;
re-measure at slice start per Rule 6).  The re-priced
row is **+80 to +120**, anchored to Phase 3's measured +101 pmccabe as
the closest control-flow comparable.  Nothing can land before a funding
Decision raises all three caps — including `SCC_COMPLEXITY_MAX`, which
the price table's own instruction forgot to name and which is the
*tightest* of the three at 2 points free.

It also exists because the design work is genuinely decided-elsewhere:
`fe/doc/unwind-design.md` already contains the five completion kinds,
the checkpoint model, and the cleanup-registry design, and the parent
mandates extending it rather than re-deriving it.  But the audit found
the document is wrong about the one behaviour Phase 6 most needs pinned
(the cleanup-raise policy — below), so this slice's job is to reconcile
the design with the measured oracle before any slice implements either.

## Parent-plan and design-doc corrections to record

Verified against the audited tree and Emacs 31.0.90, 2026-08-05:

- **`fe/doc/unwind-design.md`'s "Emacs discards" claim is false.**  The
  document asserts a cleanup's own error should be discarded "as Emacs
  does"; measured, Emacs lets the **new error replace** the in-flight
  one — `(condition-case e (unwind-protect (error "orig") (error
  "cleanup")) (error e))` → `(error cleanup)`, and a cleanup's `throw`
  likewise wins over the in-flight throw.  fe today does a third thing:
  print to stderr and keep unwinding with the original.  Decision 4
  settles the target; the design doc is updated in this slice either
  way, because a design doc that misquotes its oracle is worse than
  none.
- **The completion enum already exists, three-fifths dead.**
  `FeCompletion` (`fe_internal.h:370-376`) has all five kinds; only
  `Normal` and `Error` are ever assigned, and the enum today serves as
  a one-bit "draining?" flag read exactly once (`AllocateFrame`'s
  `CleanupFrameReserve` gate, `fe_eval.c:654-655`).  Phase 6 is not
  adding the enum — it is making the other three values true, and the
  reserve gate silently widens to them the moment they are assigned
  (a live coupling nobody has yet had to consider; 06C/06D must treat
  it as a decision, not an accident).
- **The checkpointed drain already exists.**  `RunCleanupsDownTo()`
  (`fe_eval.c:132-137`) is live and called by every completing pair
  frame; the drain-to-zero `RunCleanupsAfterError()` is the only thing
  `catch` has to displace.  The design doc's "this has to change"
  paragraph is already half-built.
- **The parent's initial condition list names two conditions with zero
  producers.**  `args-out-of-range` and `file-error` have no fe raise
  site today (the census: 27 condition-named sites across 8 names, 70
  bare-prose).  They enter the static hierarchy anyway — kg's
  `substring`/file natives are their eventual producers — but no fe
  test can exercise them until a producer exists, and the slice tests
  must not pretend otherwise.
- **A Lisp-callable `signal`/`error` cannot be an ordinary native.**
  `FeHandleError` never returns (`doc/TODO.md:206-209` flags exactly
  this); the raise *is* the primitive's behaviour.  They are
  evaluate-arguments-then-raise forms, which the frame machine can
  spell as an EvalList-shaped arm whose completion never delivers.
- **`(/ 1.0 0)` is `1.0e+INF` in Emacs, not `arith-error`.**  Phase 5
  already matches; the phase must not "improve" float division into an
  error while wiring `arith-error` up as a real condition.

## The oracle answer table

One `compat/cases/cond-*.json` + snapshot per row, all `planned`, each
rationale naming Phase 6 and the implementing slice.  Predictions below
were measured against the pinned Emacs 31.0.90 during the 2026-08-05
audit — the snapshot generation should confirm, not discover.  The
audit's full transcript (condition-case, catch/throw, signal,
unwind-protect interactions, the hierarchy table, uncaught-batch
behaviour) is the source; the rows below are the load-bearing subset.

| # | Case | Measured Emacs answer | Pins |
|---|---|---|---|
| CC1 | `(condition-case nil 42 (error 'h))` | `42` | body value passes through |
| CC2 | `(condition-case e (error "boom") (error e))` | `(error boom)` | the condition object shape, var bound |
| CC3 | `(condition-case nil (error "b") (error 1 2 3))` | `3` | handler body is an implicit progn |
| CC4 | `(condition-case v (signal 'arith-error '(7)) (arith-error v))` | `(arith-error 7)` | signal data reaches the handler |
| CC5 | `(condition-case nil (car 5) (error 'caught))` | `caught` | **hierarchy**: wrong-type-argument is-a error |
| CC6 | first *matching* handler wins, textual order | `(… (wrong-type-argument 'wta) (arith-error 'ae) …)` → `ae` | handler selection |
| CC7 | `(condition-case nil (signal 'arith-error nil) (wrong-type-argument 'wta))` | re-signals unchanged | unmatched falls through |
| CC8 | `(condition-case nil (/ 1 0) ((wrong-type-argument arith-error) 'multi))` | `multi` | a handler names a **list** |
| CC9 | `(condition-case nil (error "x") (t 'caught))` | `caught` | `t` catches everything |
| CC10 | `(condition-case nil (signal 'quit nil) (error 'e))` / `(quit 'q)` / `(t 't-h)` | signal / `q` / `t-h` | **quit: not under `error`, catchable by name and by `t`** |
| CC11 | `(condition-case v 5 (:success (list 'succ v)))` | `(succ 5)` | `:success` exists in 31.0.90 — Decision 3 scopes it |
| CT1 | `(catch 'tag (throw 'tag 7) 99)` | `7` | value passing |
| CT2 | `(catch 'a (catch 'a (throw 'a 1)) 2)` | `2` | **innermost** same-tag wins |
| CT3 | `(throw 'nowhere 1)` | signals `(no-catch nowhere 1)` | uncaught throw is an *error* |
| CT4 | `(catch nil (throw nil 5))` | `no-catch` | nil tag never matches |
| CT5 | tags: fixnum `5` matches, `1.5` and `"s"` do not, a shared cons does | eq, not eql/equal | tag comparison is `eq` — Phase 5's integer `eq` is load-bearing |
| CT6 | `(condition-case nil (throw 'tag 1) (error 'e))` | `e` | `no-catch` is under `error` |
| CT7 | throw through `unwind-protect` runs the cleanup | `(catch 'tg (unwind-protect (throw 'tg 'thrown) (push 'c l)))` → cleanup ran | cleanups on the throw path |
| S1 | `(signal 'arith-error '(1 2))` caught as `e` | `(arith-error 1 2)` | signal's object construction |
| S2 | `(signal 42 '(1))` | `(wrong-type-argument symbolp 42)` | non-symbol condition |
| S3 | `(signal 'nonexistent-condition '(1))` | `(error Invalid error symbol …)`, caught by `(error …)` | unregistered symbols |
| S4 | `(signal 'error 42)` | `(error . 42)` accepted | improper data tolerated |
| U1 | cleanup runs on error, throw, and quit paths | measured, all three | the five-kinds table |
| U2 | a raising cleanup **replaces** the in-flight error | `(error cleanup)` wins | Decision 4's oracle |
| U3 | nested unwind-protect: innermost cleanup first | `(1 (out in))` | ordering |
| Q1 | uncaught error in batch: exit 255, message + `Error:` line + backtrace on stderr; uncaught quit: no backtrace, no `Error:` line | measured | what "uncaught" looks like |
| H1 | `(get 'arith-error 'error-conditions)` | `(arith-error error)` | the hierarchy rows (full table in the audit: 24 symbols, deepest chain 4, everything Phase 6 needs is depth ≤ 2; `quit` is **not** under `error`) |
| H2 | `(error-message-string '(wrong-type-argument listp 5))` | `Wrong type argument: listp, 5` | rendering — note Emacs' `’` in void-function/-variable texts; fe pins ASCII |

Emacs condition objects worth pinning as data-shape rows: `(car 5)` →
`(wrong-type-argument listp 5)`; `(/ 1 0)` → `(arith-error)` with **no
data**; `(error "fmt %d" 7)` → `(error "fmt 7")` — formatted at signal
time; `(nosuchfn)` → `(void-function nosuchfn)`; `(throw 'x 1)` at top
level → `(no-catch x 1)`.

## Decision 1 — the condition object and the static hierarchy

To record: a condition is the cons `(SYMBOL . DATA-LIST)` fe already
almost spells in its message text, constructed at signal time.  The
hierarchy is a **static table in fe** (parent: "without requiring
general symbol properties") — an array mapping condition symbol →
parent chain, initialised at bootstrap with the parent's initial list:
`error`, `wrong-type-argument`, `wrong-number-of-arguments`,
`void-variable`, `void-function`, `args-out-of-range`, `arith-error`,
`file-error`, plus fe's own residents (`cyclic-function-indirection`,
`invalid-function`, `no-catch`) and the two resource conditions the
parent names (evaluator-stack exhaustion, arena exhaustion — spell
them; Emacs' nearest are `excessive-lisp-nesting` and nothing).  Every
chain is depth ≤ 2 (`X → error`); Emacs' one deeper chain
(`overflow-error → range-error → arith-error`) is out of scope until a
producer needs it.  The open sub-question to decide with measurements:
where the table lives (fe.c bootstrap vs a new `fe_cond.c` — §0.2's
split precedent says measure the tax before choosing) and whether
`quit`/`budget` appear in it at all given they are not conditions
(recommended: no — they are completion kinds, not signalable symbols,
and `(signal 'quit nil)` constructs an ordinary *condition* named
`quit` exactly as Emacs treats it, distinct from the real C-g path).

## Decision 2 — what `FeHandleError`'s 97 sites become

The census: 27 sites already spell an Emacs condition name at the head
of their message (9 `wrong-number-of-arguments`, 9 `arith-error`, 5
`wrong-type-argument`, 3 `void-function`, …); 70 are bare prose; 2 are
computed (`CheckType`'s `expected %s, got %s`).  To record: the
condition-named sites become structured signals of that condition; the
prose sites become `(error "prose")` — Emacs' own shape for
`(error "…")` — with **no mass rewording** (§0.4 has no legacy
constituency, but 30 goldens and 109 kg test assertions pin the prose;
the message *text* survives as the condition's rendered form through
the existing label/offset formatter, which every golden embeds).
`CheckType` becomes a `wrong-type-argument` producer.  kg's 112 raise
sites stay prose-`error` in Phase 6 (they gain nothing from
classification until kg Lisp can catch, which 06E delivers — a later
wave can classify the funnel sites; write that in TODO, not in scope).

## Decision 3 — condition-case scope

To record: handlers by symbol, by list, and `t`; nil and non-nil `var`;
handler body as implicit progn; unmatched re-signal; `quit` catchable
only by name or `t`, never by `error` (the Emacs rule, measured).
**`:success` (CC11) is deliberately deferred** — record it as a
divergence row with its measured Emacs answer; it is new in Emacs 24.1+
and nothing in kg's target init-file corpus uses it (verify against
Phase 8's wave list before recording).  `(debug error)` handler specs:
accepted and treated as `error` (Emacs semantics without a debugger).
`ignore-errors` joins the kg prelude in 06E, not fe.

## Decision 4 — the cleanup-raise policy

Three candidate behaviours: fe today (stderr + continue with the
original), the design doc's claim (discard silently), Emacs (new error
replaces).  To record with reasons: **match Emacs — the new error
replaces the in-flight completion**, because the phase's whole point is
that handlers can rely on Emacs semantics, and the divergence is
observable from Lisp (`condition-case` around a failing cleanup).  The
three `test_api.c` assertions pinning the stderr text
(`:2962, :3031, :3080`) are rewritten by 06D, and `error_fn`'s
never-sees-cleanup-failures contract changes with it.  If the Decision
lands the other way, the divergence row must quote the measured Emacs
behaviour, not the design doc's folklore.

## Decision 5 — the host API and the standalone channel

To record: the additive host surface (the parent requires a migration
path).  Recommended shape: keep `FeErrorFn` as-is for existing hosts;
add `FeGetCompletion(ctx)` + `FeGetCondition(ctx)` (kind + condition
object valid for the duration of the callback), so kg's `handle_error`
upgrades by reading, not by re-signing.  Quit and budget reach the host
as distinct kinds and are **not** catchable by `condition-case`-by-
`error` (Q-rows).  The standalone `fe` binary needs a structured error
channel for the compat runner — today `run-fe-compat.py` derives
"condition" purely from the exit code and greps the message
(`condition_source: "message"`); 06D gives the binary a way to print
the condition symbol so `condition_source` can become `"structured"`
and `planned-quit-signal`'s checked-in `{"kind": "quit"}` oracle
becomes matchable.  Decide the exact printing contract here so the
runner and the binary move together in 06D.

## The funding Decision

Measured 2026-08-05, post-review-fix (re-measure at slice start): fe
scc **533/540**, `fe_eval.c` **326/340**, pmccabe **751/760** across
263 symbols.  The row prices **+80 to +120** anchored to Phase 3's
measured +101.  Fund from the measured floor and name all three caps —
the price table's instruction omitted the tightest: raise
`SCC_COMPLEXITY_MAX` 540 → **680**, `SCC_FILE_COMPLEXITY_MAX` 340 →
**420**, `PMCCABE_TOTAL_MAX` 760 → **900**.  Prove each live by the 03A temporary-lowering trick
(`make pmccabe-baseline` must refuse to launder an over-budget tree).
kg needs no raise: 39 points of headroom (5461/5500) against a +15 to
+30 row.
If Decision 1 puts the hierarchy in a new translation unit, the split
tax is measured (03B's precedent: +72 scc, pmccabe conserved) and the
scc raise absorbs it; say which in the Decision.

## Also in this slice

- The `fe/doc/unwind-design.md` update: mark the shipped-vs-designed
  notes this phase resolves, correct the Emacs-discards claim to the
  measured behaviour, and add the Phase 5 residue it is silent about
  (`arith-error` and int64-overflow joined the message-level names).
- New `cond-*` compat cases land as `planned` with fresh snapshots
  (regenerate nothing existing).  fe's existing
  `errors-and-non-local-exits` rows are audited: `signal-and-quit`'s
  rationale already names Phase 6 and stays `planned` until 06D;
  `void-function-error`'s `condition_source: "message"` note is the
  thing 06D upgrades.
- kg: **nothing** — no pin, no source.  The five names Phase 6 adds
  (`catch`, `throw`, `condition-case`, `signal`, `error`) are checked
  against kg's manifest inventory now: none collides with a kg native
  or prelude definition (audited 2026-08-05 — `error` is claimed by
  no one; verify at slice start, the disjointness checker is the
  tripwire at the eventual pin).

## Gates

fe: `make -C fe check`, `complexity-check`, `pmccabe-check` (at the
**new** caps, proved live), `format-check`, `compat` (new rows
`planned`, nothing regenerated).  No behaviour change: the three
`test.sh` passes and all goldens byte-identical.

kg: none.

## What this does not do

- **No implementation** — no enum assignment, no new primitive, no
  frame kind.  06B–06D.
- **No `handler-bind`, no `debug-on-error`, no debugger, no
  `signal-hook-function`** — recorded exclusions; Emacs has them, kg's
  target corpus does not need them.
- **No re-classification of kg's 112 prose raise sites** — Decision 2
  records the deferral.
- **No cleanup-registry token/cancel API** (unwind-design item 2) —
  it is Fex's need, not this phase's; it stays in the design doc as
  open work with its own note (Phase 9's robustness scope is the
  natural home; record that pointer in the doc update).
