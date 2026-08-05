# 04C — The function namespace, additively

Parent: [Phase 4](../2026-08-03-elisp-subset-and-fe-evaluator.md#8-phase-4--adopt-lisp-2-namespaces),
fe-only.

**Prerequisite:** [04B](04b-symbol-cells-behind-accessors.md).  The
function cell must exist, be rooted, and be priced before anything reads
it.

## Outcome

Everything Lisp-2 *adds* lands, behind the transitional head-resolution
rule 04A's Decision fixed: a symbol in call position resolves through the
function cell first and falls back to today's value-cell path when the
function cell is unbound.  Because no bootstrap definition moves and no
tracked program uses the new forms yet, **every existing script, golden,
compat comparison and test expectation is unchanged** — that is this
slice's correctness argument, and it is what makes 04D's cut a small,
reviewable diff instead of a big-bang rewrite.

This is the 02B precedent applied to namespaces: 02B landed core `setq`
while assignment `=` still worked; this slice lands the function namespace
while the value namespace still resolves calls.  The fallback is an
in-workstream transitional state — it ships in no release and 04D deletes
it.

## Files this slice owns

**fe:** `fe_internal.h` (the `Primitive` enum), `fe.c`
(`primitive_names[]`, `GetCoreObjectCount` adapts automatically),
`fe_eval.c` (head resolution, `DispatchPrimitive`, resume arms, the new
`funcall`/`apply` path), `fe.h` (`FeSetFunction`, `FeGetFunction`,
`FeIsFBound` — additive, no version bump yet), `test_api.c`,
`compat/features.json` (flip the §1 `planned` entries whose behaviour is
now complete, and claim their `source_name`s), new `scripts/lisp2-*.fe`
with `tests/*.out`/`*.err` goldens, `doc/language.md`, `doc/c-api.md`,
`doc/implementation.md`.

**kg:** the pin move in its own green commit.  kg's
`utils/check_lisp_compat.py` parses fe's `primitive_names[]` and requires
every name claimed by exactly one manifest entry — the fe commit's
manifest updates are what keep kg's `make check` green, so verify
`lisp-compat-check` in the pin commit rather than assuming it.

## The new primitives

Nine additions to the `Primitive` enum and `primitive_names[]` (designated
by enum index; there is no sortedness requirement in fe's table):

| Name | Kind | Behaviour (pinned by 04A's snapshots) |
|---|---|---|
| `function` | special form | `(function SYM)` → `SYM`; `(function (lambda ...))` → the closure, sharing `PFn`'s construction arm; anything else raises a clear unsupported-function-form error |
| `fset` | function | both args evaluated; writes the function cell via `SetSymbolFunction`; returns per the snapshot |
| `defalias` | function | per the snapshot — including its return value; stores its definition (object *or* symbol designator) in the function cell |
| `symbol-function` | function | reads the cell; unbound → `void-function NAME` message |
| `symbol-value` | function | reads the global value cell; unbound → `void-variable NAME` message |
| `fboundp` | function | `t`/`nil` on the function cell |
| `fmakunbound` | function | unbinds the function cell only; `makunbound` keeps its value-only meaning — the two independence cases from 04A are the tests |
| `funcall` | function-shaped special form | see below |
| `apply` | function-shaped special form | see below; last argument spread per the snapshot |

The unary/binary shapes reuse the existing `FeFrameUnary`/`FeFrameBinary`
resume kinds where arity fits; do not invent new frame kinds for leaf
primitives.  Each addition automatically raises
`GetCoreObjectCount()`/`FeMinimumArenaSize()` by one primitive object plus
one symbol's cost; record the new minimum in the same commit
(`doc/fe-upstream.md`'s copy of the figure moves with the *pin* commit).

**Designator chains.**  A function cell may hold a symbol.  Resolution —
in call position, in `funcall`/`apply`, and in `FeGetFunction` — follows
the chain iteratively, charging `EvaluationStep` per hop, so `(fset 'x 'x)`
dies on the step budget with a `cyclic-function-indirection` message
instead of hanging.  One helper, used by all three sites; do not
duplicate the loop.

## `funcall` and `apply` are evaluator work

Both must run on the frame stack — a native-style implementation that
re-enters evaluation would burn the native re-entry bound and C stack per
call, which is exactly what Phase 3 removed.  Two viable shapes, either
acceptable; pick one and record why in `doc/implementation.md`:

- **Evaluate-then-redispatch:** evaluate all operands via the existing
  `FeFrameEvalList` machinery; on completion, resolve the first result
  (designator chain if a symbol), then hand the remaining *already
  evaluated* values to the call path.  `FeCall` already has the shape for
  this — it wraps each argument as `(quote v)` and builds a call
  expression; reusing that construction from a resume arm costs a few
  conses per call and zero new invariants.
- **A dedicated apply frame kind** that carries evaluated values directly
  into `ArgsToEnv`/native dispatch without the quoted re-wrap.  Cheaper
  per call, but a new frame kind means edits to every exhaustive
  `FeFrameKind` switch (`ResumeContinuation`, `FeMarkEvaluatorRoots`,
  `IsAwaitingDelivery`, the main loop) and a new entry in the GC-per-state
  test table — price it before choosing it.

Whichever shape: `apply`'s last operand must be a proper list (clear error
otherwise), the spread must not mutate the caller's list, and both forms
must work with every callable type — lambda, native, primitive where
Emacs allows it, and designator — with the 04A snapshots as the answer
key.  Mind the 03F rooting lesson: an evaluated-operand buffer is live
across later allocations; root it the way frame fields are rooted, and
run the fuzz lane before trusting it.

## The transitional head resolution

In `RunEvaluationLoop`'s `FeFrameExpression` symbol-head arm (and only
there — variable reference is untouched): consult
`SymbolFunction`-with-designator-chain first; if unbound, fall back to the
existing `CDR(GetBound(...))` path unchanged, including its lexical-env
walk and its error shape.  Extract the resolution into a helper rather
than growing `RunEvaluationLoop` past its 14-of-22 pmccabe weight; 04D
deletes the fallback branch and the helper shrinks.

Mark the fallback with a comment naming 04D as its deletion site.  The
design comment above `HandleNonCallable` — the one describing the
void-function *fiction* over one namespace — is rewritten in 04D when the
fiction becomes fact; leave it accurate about the fallback in the
meantime.

Because function cells are all unbound until a program calls
`fset`/`defalias`, the fallback means **no existing behaviour changes**:
`(boundp 'car)` is still `t`, macros still resolve through value cells,
and every 03A trace golden and `TestPrimitiveOrder` pin stays
byte-identical.

## Tests owned by this slice

- `test_api.c`: a `TestFunctionCells` suite covering the full 04A answer
  table that is implementable without the cut — coexistence via `fset`,
  `funcall` on values/designators/lambdas, `apply` spread and its
  malformed-tail error, `symbol-function`/`symbol-value`/`fboundp`,
  the `makunbound`/`fmakunbound` independence pair, designator late
  binding, the cycle-dies-on-budget case, and context reuse after each
  error.  Also GC forced across the new resumable states (the
  evaluate-then-redispatch boundary at minimum), extending the per-state
  GC table 03D established.
- New `scripts/lisp2-basics.fe` (+ `.out`/`.err`) exercising the new forms
  end-to-end under all three passes, including `-a` — the new primitives'
  arity behaviour must be stable enough that the strict-arity pass stays
  byte-identical, per `test.sh`'s rule.
- Compat: flip to `supported` (and claim `source_name`) exactly the
  entries whose semantics are complete without the cut — the
  `funcall`/`apply`/`fset`/`symbol-function`/`symbol-value`/`fboundp`/
  `fmakunbound`/`defalias`/`function` family.  `one-namespace-boundp` and
  `reader-sharp-quote-identity` **stay divergent** — their flips are 04D's
  evidence.  `make -C fe compat` proves the supported set passes against
  checked-in snapshots.

## Gates

fe: `make -C fe check` (three passes, goldens byte-identical),
`complexity-check`, `pmccabe-check` (inside 04A's funded caps, measured
before/after per Rule 6 — this slice is the phase's biggest fe spend),
`format-check`, `compat`, and the fuzz smoke with tracked seeds — the
funcall/apply rooting is exactly the class of change 03F proved the other
lanes cannot vouch for.

kg: pin move in its own green commit; `make check` (including
`lisp-compat-check` against the new primitive names) and
`make WITH_LISP=0 clean all check`.

## What this does not do

- **No cut.**  Bootstrap stays in value cells; the fallback stays; `#'`
  still reads as identity; no script migrates; no version moves.  04D.
- **No kg-side source change.**  kg's prelude, natives, hooks and tests
  are 04E's, atomically with the cut pin.
- **No `FeNativeFn` signature change, no strict arity, no conditions.**
  The new errors are message-level names, per the standing Phase 6 rule.
- **No new frame kind unless priced.**  The redispatch shape needs none;
  if the dedicated-frame shape wins, its cost was written down first.
