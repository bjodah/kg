# 04A — Pin the Lisp-2 target semantics, and fund the phase

Parent: [Phase 4](../2026-08-03-elisp-subset-and-fe-evaluator.md#8-phase-4--adopt-lisp-2-namespaces),
fe-only plus one Decision recorded in the set README.

**Prerequisite:** none.  **This is first**, for the same structural reason
00A, 02A and 03A were: Phase 4 changes what a symbol *is*, and every
downstream slice needs three things that do not exist today — the oracle's
answers, a symbol layout with measured arena arithmetic, and funded
complexity caps (both fe gates have exactly **29 points** of headroom, and
the price table's row is +40 to +60).  A slice that discovers any of the
three mid-diff writes its own gate.

Symbols are authoritative throughout this set; line numbers were measured
on the 2026-08-05 tree (fe `60e4c9e`, kg `3e3c946`) and drift.

## Outcome

At the end of this slice, before one line of namespace logic exists:

1. Every Phase 4 semantic question has a **version-stamped Emacs 31
   answer** checked in — the parent plan's §8 differential list plus the
   cases this document adds — as `planned` entries in `fe/compat/`.
2. The **symbol layout is decided with numbers**: a compiler-measured
   spike, the per-symbol object cost, the new `GetCoreObjectCount()` and
   `FeMinimumArenaSize()`, and the effect on kg's 1 MiB arena and fe's
   64 KiB fuzz arena.
3. The **host C API surface is named**: which functions keep their
   meaning, which change it, which are added, and why `FE_API_VERSION`
   2 → 3 is the tripwire that makes the one meaning-change safe.
4. The **migration mechanics are decided up front**: the transitional
   head-resolution rule 04C ships and 04D deletes, the prelude spelling
   04E rewrites to, and the two kg checkers that spelling breaks.
5. A dated **Decision** in the set README funds the phase in both trees,
   re-measured per Rule 6.

No behaviour changes anywhere.  Every new manifest entry is `planned`;
every existing `divergent` entry stays divergent.

## Files this slice owns

| File | Why it changes |
|---|---|
| `fe/compat/features.json`, `fe/compat/cases/*.json`, `fe/compat/oracle/*.json` | The new `planned` Lisp-2 entries, their cases, and their Emacs 31 snapshots. |
| `fe/compat/README.md` | One paragraph: `planned` entries with snapshots are Phase 4's contract, mirroring 02A's precedent. |
| `fe/Makefile` | The funded cap raises (`SCC_COMPLEXITY_MAX`, `SCC_FILE_COMPLEXITY_MAX`, `PMCCABE_TOTAL_MAX`), with the rationale comment extended. |
| this sub-plan and the set `README.md` | The predicted-answer table's outcome, the layout arithmetic, and the dated Decision. |

There is no production `fe.c`/`fe_eval.c`/`fe.h` change in the landed
slice.  The layout spike is throwaway and is deleted before commit, like
00A's and 03A's.

## 1. The differential corpus

02A's method, applied to namespaces: write the predicted table first, then
run `make -C fe compat-oracle` (resolution order `--emacs`, `$KG_PTY_EMACS`,
PATH, the `/opt-3` pin) and record what Emacs actually says.  Every case is
`owner: fe-core`, `comparison: emacs`, `status: planned` with a rationale
matching `Phase 4` (the kg-side checker enforces that spelling), and
`source_name: null` until the primitive exists.

Predicted answers, to be confirmed or corrected by the oracle — a wrong
prediction is a finding, not a failure:

| Case id | Setup / expr | Predicted |
|---|---|---|
| `lisp2-value-function-coexist` | `(setq f 7)` `(fset 'f (lambda () 9))` → `(list f (f))` | `(7 9)` — the parent plan's headline |
| `lisp2-let-no-function-shadow` | `(let ((car 5)) (car (list 1 2)))` | `1` — `let` binds the value namespace only |
| `lisp2-funcall-lexical-value` | `(let ((g (lambda (x) (+ x 1)))) (funcall g 3))` | `4` |
| `lisp2-funcall-designator` | `(setq g 7)` `(fset 'g (lambda () 9))` → `(funcall 'g)` | `9` — a symbol designator resolves through the function cell |
| `lisp2-apply-spread` | `(apply '+ 1 2 (list 3 4))` | `10` |
| `lisp2-fboundp-primitive` | `(fboundp 'car)` | `t` |
| `lisp2-functionp-symbol` | `(functionp 'car)` / `(functionp 'no-such)` | `t` / `nil` — kg's `functionp` native must learn designators (04E) |
| `lisp2-void-function-value-only` | `(setq v 7)` → `(v)` | `void-function v` |
| `lisp2-symbol-function-roundtrip` | `(fset 'h (lambda (x) x))` → `(funcall (symbol-function 'h) 5)` | `5` — avoids comparing printed closures |
| `lisp2-symbol-value` | `(setq sv 5)` → `(symbol-value 'sv)`; also `(symbol-value 'car)` | `5`; `void-variable car` |
| `lisp2-fmakunbound` | `(fset 'j (lambda () 1))` `(fmakunbound 'j)` → `(j)` | `void-function j` |
| `lisp2-makunbound-keeps-function` | `(setq m 1)` `(fset 'm (lambda () 2))` `(makunbound 'm)` → `(m)` | `2` |
| `lisp2-fmakunbound-keeps-value` | same setup, `(fmakunbound 'm)` → `m` | `1` |
| `lisp2-defalias-symbol` | `(defalias 'first 'car)` → `(first (list 1 2))` | `1` — and record `defalias`'s own return value |
| `lisp2-defalias-late-binding` | `(defalias 'a 'b)` `(fset 'b (lambda () 1))` → `(a)` | `1` — function-cell symbol indirection is resolved at call time, not at `defalias` time |
| `lisp2-function-lambda` | `(funcall (function (lambda (x) (+ x 1))) 2)` | `3` |
| `lisp2-fset-both-args-evaluated` | `(setq s 'k)` `(fset s (lambda () 4))` → `(k)` | `4` |

Plus the two existing divergent entries whose snapshots **already record
the target**: `one-namespace-boundp` (`(boundp 'car)` → `nil`) and
`reader-sharp-quote-identity` (`(quote #'car)` → printed `#'car`).  Do not
touch them here; 04D flips them.  kg's own `one-namespace-clobber` and
`prelude-function` entries flip in 04E.

Three questions the corpus must settle *as data*, because the answer
constrains implementation:

- **Printed closures.**  What does batch Emacs 31 print for an interpreted
  `(lambda (x) x)` value?  If the printed form is a closure/bytecode object
  kg cannot reproduce, cases must be shaped to compare *behaviour*
  (roundtrip through `funcall`) rather than printing, or be `kg-policy`.
  The roundtrip cases above are already shaped that way; keep them so.
- **`#'` printing.**  The pinned snapshot says Emacs prints `(function
  car)` as `#'car`.  Confirm what fe's writer does with `(quote x)` today
  and record whether 04D therefore owes the writer a `(function X)` → `#'X`
  abbreviation to make the flip honest.
- **Macro representation.**  Emacs stores a macro as `(macro . FUNCTION)`
  in the function cell; fe has a distinct `FeTMacro` type.  Record this as
  a `kg-policy` representation divergence now (observable only through
  `symbol-function` of a macro), so 04C does not have to imitate the cons
  representation to pass a case nobody wrote yet.

Message-level conditions, per the Phase 2 rule restated by the parent plan:
`void-function`, `void-variable` and `cyclic-function-indirection` are
*names carried in the `FeHandleError()` message*, not signalable condition
objects, until Phase 6.  No case may assert a catchable condition.

## 2. The symbol layout, decided with numbers

Today (`fe/fe.c`, `FeMakeSymbol`): a symbol object's cdr is one cons,
`CDR(sym) = (name . value)`.  `GetBound`'s global path returns that cons as
the binding cell; every reader does `CDR(cell)`, every writer does
`CDR(cell) = v`.  Interning walks `ctx->symbol_list`; `GetSymbolObjectCount`
prices a symbol at `4 + (len-1)/7` objects; `GetCoreObjectCount()` is
currently **256** objects and `FeMinimumArenaSize()` **53840** bytes.

The function cell has to live somewhere reachable from the symbol without
breaking that cell contract.  Candidates, to be settled by a throwaway
compile-and-measure spike:

- **(a) — recommended — `CDR(sym) = ((name . function) . value)`.**  The
  binding cell stays `CDR(sym)` and the value stays in its cdr, so
  `GetBound`, `FeSet`, `FeIsBound`, `ResumeSetq`, lexical-env symmetry and
  the whole value path are untouched by layout.  The name moves one level
  down (`CAR(CAR(CDR(sym)))`), which touches exactly the sites 04B wraps in
  accessors: the `FeMakeSymbol` intern scan, `GetStringObject`,
  `IsNamedSymbol`, the writer's `FeTSymbol` arm, and the `fn`-alias
  registration.  GC needs **zero** change: `FeMark`'s `FeTSymbol` arm walks
  `CDR(obj)`, reaches the new inner pair as an ordinary `FeTPair`, and marks
  name, function and value from there.  Cost: **+1 object per symbol**.
- **(b) `CDR(sym) = (name value . function)` or similar re-chaining.**
  Same object cost, but it moves the *value* cell, which rewrites the
  `GetBound` contract and every caller — strictly more blast radius for
  the same price.  Reject unless the spike finds a concrete problem
  with (a).
- **(c) Pointer bits in the symbol's car.**  The car holds the type tag
  and GC mark bit; stealing its pointer payload would need tagging tricks
  and a writer/GC audit.  Zero object cost, highest risk.  Reject unless
  arena arithmetic shows (a) is unaffordable — it will not (below).

The arithmetic to verify in the spike, not assert:

| Quantity | Today | Under (a), predicted |
|---|---|---|
| objects per symbol | `4 + (len-1)/7` | `5 + (len-1)/7` |
| `GetCoreObjectCount()` | 256 | ≈307 (+51: `t`, 33 primitives, `fn`, 16 math names) |
| `FeMinimumArenaSize()` | 53840 B | ≈54656 B (+816) |
| kg 1 MiB arena | 1100 frames / 56210 slots | frames unchanged, ≈−50 slots at open, plus one object per interned symbol at runtime |
| fe 64 KiB fuzz arena | — | re-measure; this is the tightest arena in the tree |

kg's measured margin makes the runtime cost ignorable in advance
(`lisp_arena_peak_live` 3132 against ~56k slots, per 00D's counters), but
the Decision must say so with the re-run number, not by citing the old one.
Note that `GetCoreObjectCount()` iterates the primitive-names array, so
04C's new primitives raise it automatically; the constant to watch is the
*minimum* arena and `TestContextCreation`'s boundary probe, which adapt by
construction.

## 3. The host C API

kg calls **no** global-binding API today — no `FeSet`, no `FeIsBound`
anywhere in `src/` — its entire binding traffic is `FeDefineNative` (78
natives) plus one `FeEvaluate` of a bare symbol in
`src/lisp_hooks.c:resolve_hook_function`.  That makes the API decision
cheap, and it should be taken here so 04C/04D implement rather than
design:

| API | Disposition |
|---|---|
| `FeSet`, `FeIsBound` | Keep, value namespace — that is their Emacs meaning (`set`, `boundp`). |
| `FeSetFunction`, `FeGetFunction`, `FeIsFBound` | New in 04C.  `FeGetFunction` is what kg's hook/process resolution uses in 04E; it follows function-cell symbol indirection the way call-position lookup does. |
| `FeDefineNative` | **Meaning changes in 04D**: it registers into the function cell.  Keep the name; the `FE_API_VERSION` 2 → 3 bump plus kg's `static_assert` is the tripwire that makes a silent-behaviour rename argument moot — kg cannot compile against the new header without visiting the assert. |
| `FeCall`, `FeCallWithOptions`, roots | Untouched — they take callable objects, not names. |

Record explicitly that the parent plan's "remove the ambiguous entry
points" clause resolves to **nothing removed**: after the split, `FeSet`
is unambiguously the value namespace and no dual-meaning entry point
exists.  Function-cell contents may be a symbol (a designator, per the
`defalias`-late-binding case); call-position lookup follows the chain
iteratively, charged against the step budget, so a cycle dies on the
budget with a `cyclic-function-indirection` message rather than hanging.

## 4. The migration mechanics, decided up front

Four decisions the later slices execute:

1. **The transitional head-resolution rule.**  Between 04C and 04D, a
   symbol in call position resolves through the function cell first and
   falls back to today's `GetBound` path.  This is the 02B precedent — an
   in-workstream transitional state, never a released coexistence layer —
   and 04D deletes the fallback.  It exists so 04C is additive (every
   existing script and golden unchanged) and the cut is one reviewable
   diff.
2. **The prelude spelling.**  04E rewrites `lisp/prelude.el`'s 53
   column-zero `(setq NAME ...)` forms to `(defalias 'NAME ...)`.  Two kg
   gates parse that shape and must move in the same commit:
   `utils/check_lisp_compat.py`'s `parse_kg_prelude_defs` (regex
   `^\(setq NAME`) and `test/test_lisp.c:test_prelude_source_file`
   (`PRELUDE_DEFS`, shape-to-`type-of` inference, the `internal--let`
   ordering pin).  Say so in 04E's checklist; a missed one fails
   `make check`, which is the good outcome — the bad outcome is editing
   the checker without re-deriving what it should now assert.
3. **`internal--let` survives, and the parent plan's sentence about it is
   wrong.**  §8 claims Lisp-2 makes the alias unnecessary; it does not —
   the prelude's Emacs `let` macro and Fe's one-binding `let` primitive
   are *both* function-namespace residents, so the redefinition still
   clobbers the cell the bodies need.  `internal--let` becomes
   `(defalias 'internal--let (symbol-function 'let))` evaluated before the
   redefinition, and is deleted in Phase 8 Wave A when `let` moves into
   core, exactly as the parent's own sequencing note asks.  Record the
   correction in the set README so 04E does not re-derive it under time
   pressure.
4. **Where bootstrap definitions live at each stage.**  04C leaves the
   bootstrap (primitives, `fn`, math natives) in value cells — the
   fallback finds them, nothing moves.  04D moves all of them to function
   cells and leaves `t`, `pi` and `e` as values.  `(boundp 'car)` flips
   from `t` to `nil` at 04D, which is exactly what the pinned oracle
   snapshot has been waiting for.

## 5. Complexity: measure and fund

Re-measured starting state (2026-08-05): fe scc **391/420** total with
`fe_eval.c` at **208/240** file cap; pmccabe **601/630** across 230
symbols, worst function 14 (`Read`, `RunEvaluationLoop`); kg **5444/5500**.
The price table's Phase 4 row is fe **+40 to +60**, kg **+15 to +25**.

What the estimate must now cover, priced against the shape of the real
tree rather than the row's original guess: ~9 new primitives (`function`,
`fset`, `symbol-function`, `symbol-value`, `fboundp`, `fmakunbound`,
`defalias`, `funcall`, `apply`) landing in `DispatchPrimitive` and the
resume arms; the head-resolution fork in `RunEvaluationLoop` (already at
14 of the 22 per-function cap — budget for extracting a helper rather
than growing the switch); designator-chain resolution; and 04B's
accessor layer, which is near-free in pmccabe but not zero.  `funcall`/
`apply` are evaluator work, not leaf primitives; price them against
`ResumeEvalList`'s weight, roughly doubled.

The caps are raised **here**, ahead of the work, per 00A/03A precedent:
`PMCCABE_TOTAL_MAX` and `SCC_COMPLEXITY_MAX`/`SCC_FILE_COMPLEXITY_MAX`
move by the estimate the spike supports, stated as funding 04B–04D by
name.  pmccabe remains the authoritative unit for the core (03A's
Decision); price and report in both units anyway.  kg's estimate (+15 to
+25 against 56 points of measured headroom — re-measure it) needs no
raise; say so.

## The Decision this slice must write

Into the set README, dated, in the shape 00A/03A established:

- the corpus outcome: which predictions held, which the oracle corrected,
  and the three data questions from §1 (printed closures, `#'` printing,
  macro representation) answered;
- the symbol layout, with the spike's measured `sizeof` arithmetic, the
  new minimum-arena figure, and both arenas' headroom;
- the API table from §3, including the "nothing removed" finding and the
  designator-indirection rule;
- the four migration mechanics from §4, including the parent-plan
  correction about `internal--let`;
- the funded caps, both trees, with the named deliverables they fund.

## Tests owned by this slice

- The new compat cases and their version-stamped snapshots, verified by
  `make -C fe compat` (checked-in snapshots, no Emacs needed) and a full
  `make -C fe compat-oracle` run that regenerates nothing unexpectedly —
  02A's own gate, including the id-collision and schema checks in both
  manifests.
- No fe unit test changes: nothing executable changed.

## Gates

`make -C fe check`, `make -C fe compat`, `make -C fe complexity-check`,
`make -C fe pmccabe-check` (green against the *raised* caps, measured
unchanged).  kg: `make check` and `make WITH_LISP=0 clean all check`
after the pin move — the pin commit is trivial (compat data plus Makefile
caps), per Rule 10.

## What this does not do

- **No implementation.**  Not one primitive, not one accessor, not one
  reader change.  The spike is deleted.
- **No status flips.**  Every existing `divergent` entry stays divergent;
  every new entry is `planned`.  Flips happen in the slice that lands the
  behaviour, with the flip as its evidence.
- **No kg-side manifest or prelude edits.**  kg's entries move in 04E,
  with the code they describe.
- **No Emacs-oracle regeneration of unrelated snapshots.**  The version
  guard exists for this; `--allow-version-change` stays unused.
