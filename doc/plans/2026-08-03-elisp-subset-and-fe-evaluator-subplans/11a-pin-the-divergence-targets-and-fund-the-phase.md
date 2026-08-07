# Sub-plan 11A — Pin the divergence targets, extend the parent, fund the phase

Phase 11 is the first phase *after* the parent program:
`doc/plans/2026-08-03-elisp-subset-and-fe-evaluator.md` closed with
Phase 10 (its §18 checklist is asserted by the milestone gate table in
`test/lisp-compat/README.md`), and this set continues the same numbered
series against the same rules, retiring the sharpest divergences the
program *recorded* rather than fixed.  First of the eleventh set; no
prerequisites.  Like every A-slice it changes no behaviour: it records
what is true, decides what each fix means, and funds the work.  Every
number below was measured 2026-08-07 at kg `2cbccbc` / fe `6355f7f` by
the Phase 11 fact audit; per Rule 6 the implementing slices re-measure
at slice start.

## What Phase 11 is

Four recorded `comparison: emacs` divergences whose common property is
that they are the ones a user *hits* rather than reads about — three
change an answer or a control path silently, one is visible in every
`M-:` echo:

1. **`defvar` does not mark a symbol special, and `let` over a special
   is lexical** (`prelude-defvar-special`).  Measured headline:
   `(defvar hkv nil) (defun callee () hkv) (defun caller () (let ((hkv
   t)) (callee)))` — Emacs `t`, kg `nil`, silently.  This is *the*
   Emacs temporary-setting idiom, and every init file that uses it gets
   a silently different program.
2. **The writer never abbreviates `(quote x)` → `'x`**
   (`writer-quote-abbreviation`) — while `(function f)` → `#'f` has
   been implemented since Phase 4 (`fe/fe.c:1090-1097`) and is the
   template.
3. **An ordinary error inside a loaded file escapes an enclosing
   `condition-case`** (`load-error-condition-reachability`) — the
   nested `FeEvaluateString` at `src/lisp_io.c:763` is the only kg
   native re-entry dressed as a top-level call.
4. **A `throw` does not cross `save-excursion` /
   `with-current-buffer`** (`catch-throw-reachability`) — measured:
   `condition-case` *already* crosses both (06E's protected-call work),
   only `throw` does not, because a failed catch search is converted to
   a `no-catch` error at `fe/fe_eval.c:3351` before anything can
   contain it.

## The audit's grid, and the corrections it forced

The 21-probe Emacs 31.0.90 semantics grid (lexical-binding: t,
top-level forms — a `progn` wrapper changes `defvar`'s answers and must
not be used when re-measuring) is preserved in the table below.  Six
probes **already agree** and are regression guards, not work: A2b
(`setq` outside `let`), **A4**, A6b, A10b (one-arg `defvar` leaves the
symbol unbound), A12 (`defvar` first-wins), A13 (plain variables stay
lexical).

Two premises of the phase's own first draft were falsified by
measurement and are corrected here so no implementer inherits them:

- **A defun parameter named after a special variable is bound
  LEXICALLY in Emacs 31 under `lexical-binding: t`** — confirmed
  interpreted, byte-compiled, with a user `defvar` and with core
  `case-fold-search`; only `lexical-binding: nil` binds it
  dynamically.  kg already agrees (`(taker 9)` → `(9 1)` both sides).
  An implementation that binds specials dynamically at *every*
  parameter-binding site would close one divergence and open this one.
- **The two "host barrier" divergences share a root cause but not a
  fix.**  Both `FindConditionHandler` (`fe/fe_eval.c:238`) and
  `FindCatchFrame` (`fe/fe_eval.c:3302`) stop at the same
  `ctx->run_base` floor (written once, `fe_eval.c:3808`).  But a
  condition survives to be replayed by `FeResignal`, while a throw is
  converted to a `no-catch` error first — so `catch-throw-reachability`
  is closed **kg-side, with no fe change**, by expanding the two
  prelude forms to Lisp `unwind-protect` (the fix the manifest row
  itself names), and only the load barrier needs a new fe entry point.

The divergence grid Phase 11 flips (Emacs answer first, kg today second):

| Probe | Form | Emacs | kg |
|---|---|---|---|
| A1 | `(defvar dvs 1) (defun dvsf () dvs)` ⊢ `(let ((dvs 2)) (dvsf))` | `2` | `1` |
| A2a | `(defvar sv 1) (defun svf () sv)` ⊢ `(list (let ((sv 2)) (setq sv 3) (svf)) sv)` | `(3 1)` | `(1 1)` |
| A3a | nested lets over a special | `(1 2 1)` | `(0 0 0)` |
| A3b | `let*` with a special | `(2 2)` | `(2 1)` |
| A5 | lambda closing over a special reads the dynamic value at call time | `2` | `1` |
| A6a | binding visible until `throw` unwinds | `(2 1)` | `(1 1)` |
| A7a/A7b | one-arg `(defvar v)`: `special-variable-p` `nil`, yet `let` **is** dynamic | `nil` / `5` | void-function / `unbound` |
| A7d/A8a | two-arg `defvar` / `defconst` → `special-variable-p` `t` | `t` | void-function |
| A8b | `defconst` is let-rebindable | `(2 1)` | `(1 1)` |
| A9a | `special-variable-p` exists | `t` | `nil` |
| A10a | let over an *unbound* special binds it, unboundness restored | `((bound 3) unbound)` | `(unbound unbound)` |
| A11 | the temporary-setting idiom | `t` | `nil` |

## Decisions

1. **Phase 11 is the four recorded divergences above, and nothing
   else.**  No buffer-local variables (they need a per-buffer binding
   table and a lifetime rule for buffer kill/switch — their own phase;
   `setq-local`/`setq-default` stay recorded aliases and their manifest
   rows stay `divergent`), no `default-value`/`set-default`/
   `make-local-variable`, no backquote printing (Decision 4), no
   `macroexpand-all`, no missing-function channel.
2. **Dynamic binding is shallow binding, consulted at fe's
   binding-list paths only.**  A special symbol carries two new flags:
   *special* (set by two-arg `defvar` and by `defconst`;
   `special-variable-p` answers it) and *let-dynamic* (set by both
   defvar arities; the flag `let`/`let*` consult).  The one-arg
   `defvar` quirk is thereby reproduced exactly as measured
   (A7a `nil`, A7b dynamic) — with the honest approximation, recorded
   in the manifest rationale, that kg's marking is global where Emacs
   scopes a one-arg `defvar` to the enclosing file.  Binding a special
   swaps the global value cell and records the old value (or its
   unboundness — A10a) in the binding frame; the frame's teardown
   restores it on **all** completion kinds (normal, error, throw, quit,
   budget), which is what makes A6a/A6b both hold.  Shallow binding
   makes A2a (`setq` writes the innermost binding) and A5 (closures
   read the current dynamic value) correct with no extra machinery.
   Closure/defun **parameter binding stays lexical unconditionally**
   (the A4 guard).  Marking is exposed to Lisp as a core
   `internal--mark-special` primitive (arity 2: symbol, `t` for full
   special / `nil` for let-dynamic-only) plus the Emacs-named
   `special-variable-p`; kg's prelude `defvar`/`defconst` macros call
   the former.  fe gains no `defvar`.
3. **kg's prelude `let` moves onto fe's Emacs-shaped core `let`.**
   Today `lisp/prelude.el:303-311` expands `let` to a lambda
   application and `let*` to `internal--let` chains — so the special
   flag would otherwise have to be consulted at parameter binding,
   which Decision 2 forbids.  fe's `DispatchLet` already accepts an
   Emacs-shaped bindings list (`StartBindingLet`,
   `fe/fe_eval.c:1120-1144`); 11B teaches that path and `internal--let`
   the special-flag consultation, and 11D deletes the prelude `let`
   macro in favour of the core form.  The 53 generically-named
   `internal--let` temporaries in the prelude (`result`, `res`, `doc`,
   `name`, …) become dynamically capturable the moment a user
   `defvar`s such a name — **which is exactly Emacs' own exposure for
   lexical-binding libraries, so it is recorded, not defended.**
4. **The writer abbreviates `(quote X)` → `'X`; backquote printing is
   out of scope.**  The change is the ~8-line symmetric copy of the
   existing `function` block at `fe/fe.c:1090-1097`, same
   single-element-proper-form discrimination (`(quote x y)`,
   `(quote)`, `(quote . x)` all keep printing as pairs — measured
   Emacs behaviour), recursive so `''x` and `(a 'b c)` come out as
   Emacs prints them.  Backquote is different in kind: kg's reader
   expands to the ordinary symbols `quasiquote`/`unquote`/
   `unquote-splicing` where Emacs uses `` \` ``/`\,`/`\,@`, and Emacs'
   comma abbreviation is context-sensitive — closing it means changing
   the *reader's expansion symbols*, breaking any Lisp that
   pattern-matches on `quasiquote`.  It stays the recorded
   `phase8-reader-backquote-symbol-names` divergence, rationale
   updated to say the quote half is now closed.
5. **The load barrier gets Shape A — containment plus resignal — and
   the throw half stays a recorded divergence.**  fe gains
   `FeTryEvaluateStringWithOptions`, a sibling of
   `FeTryCallWithOptions` (`fe/fe_eval.c:4246-4306`) with
   `EvaluateInput` in place of the call: a non-normal completion is
   returned, not thrown past the frame.  kg's `lisp_eval_file()`
   (`src/lisp_io.c:745`) uses it and re-raises via `FeResignal` after
   the enclosing run's floor is back, so an enclosing `condition-case`
   is found — closing `load-error-condition-reachability` exactly as
   its manifest row asks.  A `throw` out of a loaded file to a `catch`
   around `(load …)` (Emacs: reaches it) would need Shape B — `load`
   as an fe primitive with its own frame kind — and is **rejected by
   scope**: it gets a new `divergent` row with a case pinning the
   measured `no-catch` answer, and a `doc/TODO.md` entry.  Two
   incidental measured divergences ride along: `load` returns `nil`
   where Emacs returns `t` — **fixed** in 11D (one line, oracle case);
   kg raises plain `error` where Emacs raises `file-missing` for a
   missing file — **recorded**, not fixed (condition-subtype work).
6. **`catch-throw-reachability` is closed in the prelude, with the
   editor state captured by two small kg natives.**  `save-excursion`
   and `with-current-buffer` become prelude macros expanding to Lisp
   `unwind-protect` around the body with paired capture/restore
   natives (`src/lisp_buffer.c`), so no native frame stands between a
   `throw` and its `catch`.  The regression guards: `condition-case`
   must still cross both (it does today — keep the measured case), and
   the remaining native walls (hooks, process filters, nested
   `command-execute`) stay walls — the manifest rows and
   `README.md:587-590` are re-worded to name the two forms that now
   pass and the callback walls that remain.
7. **The XPASS rule is the phase's sequencing constraint, on both
   sides.**  kg's oracle runner fails a `divergent` case that starts
   agreeing, so **every behaviour flip and its
   `test/lisp-compat/features.json` edit must land in the same
   commit** (`prelude-defvar-special`, `writer-quote-abbreviation`,
   `load-error-condition-reachability`, `catch-throw-reachability`).
   fe's runner has no XPASS rule and will *silently* stop being right:
   the fe slices must edit fe's own affected rows by hand —
   `phase7-arity-condition-rendering` (splits: the quote half agrees,
   the `#[...]` byte-code half stays divergent),
   `phase7-primitive-quote-arity` (the `-nargs` workaround note),
   `primitive-quote-too-many` (`compare_data: false` opt-out becomes
   unjustified), `primitive-let` (rationale already stale — it
   describes a fe whose `let` had no bindings-list arm),
   `primitive-setq`/`primitive-set` (gain an "unless special" clause).
   Existing oracle snapshots are never regenerated; new cases get
   fresh runner-produced snapshots.
8. **Funding (bases re-measured at the Phase 10 acceptance,
   2026-08-07).**  kg scc **5802/5802**; fe scc **787/787**; fe
   pmccabe **1088/1088** (347 symbols); `fe_eval.c` **517/520** file
   cap; kg has no pmccabe total cap and its binding constraint is the
   scc total; both trees cap a *new* function at pmccabe 15, and fe's
   worst existing function is exactly 15 — new fe functions must be
   small or split.  Raises: **fe scc 787→835 and pmccabe 1088→1140 in
   11B's opening commit** (dynamic binding ~+15..30, protected entry
   ~+8..15, writer ~+2..4); **kg scc 5802→5825 in 11D's opening
   commit** (capture/restore natives, the loader seam, `load`
   returning `t`; +8..20 band).  Each raise with temporary-lowering
   proof; 11C re-sets fe's caps at measured actuals **pre-pin** (the
   Phase 9/10 ordering rule), 11E re-sets kg's at close.
   **The `fe_eval.c` file cap is handled by a translation-unit split,
   not a raise**: 11B's opening commit moves the trailing public-API
   block (`fe_eval.c:4027-4335`, ~309 lines: `RunEvaluation`,
   `Evaluate`, `FeCall*`, `FeTryCallWithOptions`) into a new TU so the
   520 per-file cap keeps binding at full strength.  Measured caveats
   that make this a split and not a saving: `check_scc_complexity.py`
   sums per file, so the split does **not** relieve the 787 total; and
   kg's Makefile compiles the submodule TUs by name
   (`src/fe.o src/fe_eval.o`, `Makefile:496-499`), so the new TU is a
   named 11D pin adaptation on the kg side and a Makefile/`.ci` change
   on fe's.
9. **The fuzz grammar gains one dynamic-binding arm, and the seed
   re-derivation is priced, not discovered.**  `fe/fuzz/fuzz_eval.c`
   has no defvar-family arm; adding one (mark-special + binding `let`
   + a throw/error inside, the unwind path fuzzing exists to hit)
   widens `BuildExpression`'s modulus (`% 36` → `% 37`), which
   re-steers all 14 tracked seeds silently.  Phase 9's precedent
   (`fe/utils/verify_fuzz_seeds.py:16-22`): the last widening
   invalidated 6 of 14 and forced hand re-derivation.  11B budgets
   that: re-derive per `fe/fuzz/seeds/README.md`, update
   `reachability.json` (it is hand-maintained, nothing generates it),
   and `make fuzz-eval-seed-verify` green is the gate.  The arm uses
   fe spellings (`internal--mark-special` + core `let`) since fe has
   no `defvar`.
10. **Versions move: `FE_LANGUAGE_VERSION` 8→9, `FE_API_VERSION` 6→7,
    `FeVersion` "10.0", `doc/lisp-api.md` document version 2→3.**
    Language: printed representation and binding semantics change
    (the V6 precedent — changed answers move the version).  API: a new
    public entry point (`FeTryEvaluateStringWithOptions`).  kg's
    compile-time reconciliation trips at the 11D pin as at every pin
    since 03F; the audit verified the header-dependency fix from
    `4414999` makes the tripwire effective.

## Extensions to the parent (recorded, binding for Phase 11)

1. §17 lists "dynamic binding" as excluded *"unless a proof workload
   establishes a concrete need."*  The need is now established and
   measured: the A11 temporary-setting idiom silently misbehaves, the
   shipped proof packages (`lisp/auto-fill.el`) read defvar'd
   variables free from defun bodies in exactly the shape that
   dynamic binding defines, and the divergence row Phase 10 recorded
   (`prelude-defvar-special`) is the manifest's sharpest.  The
   exclusion is lifted for the Decision-2 subset only; buffer-locals
   and `lexical-binding: nil` file semantics remain out.
2. §14/§18 are closed and stay closed: Phase 11 does not reopen the
   milestone gate table; it *moves four rows* of the divergence
   inventory the gate cites, and the gate's counts are re-recorded at
   11E.
3. The parent's §2 fidelity rule is the controlling design principle
   for the semantics grid: where the grid and any older kg document
   disagree (e.g. `doc/lisp-api.md`'s "no dynamic binding" bullet and
   its workaround advice), the grid controls and the document is
   rewritten in 11E.

## Work

1. This document, the README's Phase 11 sections (through-line,
   grouping, handoff, sequencing, deliberately-not, price rows), and
   the parent extension block after §17 — recorded, no behaviour
   change.
2. The funded raises land in 11B's/11D's opening commits, not here.
3. Re-measure the caps/baseline table at the current HEADs and record
   it in the README baseline paragraph (done in the same commit as
   this document).

## Gates

- No behaviour change: `make check` identical before/after.
- The README's Phase 11 rows and this document agree on every figure.

## Price

0 scc both trees (documentation only).
