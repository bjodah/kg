# 02A — Pin `setq`, `set` and numeric `=` against the oracle, before implementing any of them

Parent: [Phase 2](../2026-08-03-elisp-subset-and-fe-evaluator.md#6-phase-2--hard-cut-assignment-and-numeric-),
oracle work in both trees.

**Prerequisites:** none beyond the first set.  This is Phase 2's first
slice and it writes no C.

## Why this exists

The parent plan says one thing about `set` that decides the shape of this
whole phase:

> Its exact interaction with lexical bindings must be fixed by
> differential cases **before implementation**.  It must follow the pinned
> Emacs oracle rather than simply aliasing `setq`.

That sentence is not specific to `set`.  Phase 2 changes what `=` *means*,
and the only reason the change is checkable at all is that Phase 0 built a
mechanism to ask Emacs what the answer should be.  Using it after the
implementation exists turns it into a rubber stamp on whatever got
written; using it first makes the implementation a fill-in-the-blanks
exercise against recorded answers.

This slice is cheap, it is the last moment the answers are unbiased, and
it costs **zero complexity in both trees** — it adds JSON and Lisp, not C.

## The machinery already exists — use it, do not extend it

Phase 0 delivered exactly what this slice needs, and it is worth naming
the parts so this document does not re-specify them:

- `fe/compat/{features.json,cases/,oracle/}` and
  `test/lisp-compat/{features.json,cases/,oracle/}`, sharing one schema.
- `fe/utils/run-emacs-oracle.py`, which takes an explicit `--corpus-root`,
  so kg's corpus drives the same runner.
- `fe/utils/run-fe-compat.py`, which runs cases against the standalone
  `fe` binary and reports a `status`-keyed **known gap** distinctly from a
  failure.
- `make -C fe compat` (snapshot-only, no Emacs needed),
  `make -C fe compat-oracle` and `make lisp-compat-oracle` (regenerate,
  and **fail loudly** when the oracle version differs).
- `utils/check_lisp_compat.py` in `make check`, which asserts every entry
  is well-formed and every source-level definition is claimed exactly once.

**Add cases and manifest entries.  Add no runner, no format, no target.**

## The status transition this slice performs

Every construct below gets a manifest entry now, and the entry's `status`
is what carries Phase 2's progress:

| Construct | Status entering 02A | Status leaving Phase 2 |
|---|---|---|
| `setq` (core special form) | `planned`, phase 2 | `supported` |
| `set` (function) | `planned`, phase 2 | `supported` |
| `=` as numeric comparison | `planned`, phase 2 | `supported` |
| `=` as assignment | `divergent`, with a case asserting the difference | **entry deleted** |
| kg's prelude `setq` macro | `supported` today | **entry deleted** with the macro |

The last two rows are the ones to get right.  00C's inventory deliberately
has no `legacy` status: a divergence that gets removed loses its entry
rather than acquiring a promise.  `utils/check_lisp_compat.py` enforces
that every source-level definition is claimed, so deleting the prelude's
`setq` macro in 02D **must** delete its entry in the same commit or `make
check` fails — which is the intended coupling, not an obstacle.

## What the cases must pin

### `setq`

The parent plan lists the required behaviour, and every line of it is a
case:

- zero arguments returns `nil`;
- arguments occur in symbol/value pairs;
- an odd argument count raises `wrong-number-of-arguments`;
- values are evaluated **left to right** — pin this with a case whose
  second value observes the first assignment;
- an existing lexical binding is updated;
- otherwise the global value cell is updated;
- the final assigned value is returned.

Add the ones the list implies but does not say: `setq` on a symbol that is
currently unbound; `setq` inside a `let` body assigning the `let`'s own
variable, then read after the `let` exits; a `setq` whose value form
signals.

### `set`

This is the one the parent plan singles out, so treat its case list as the
deliverable rather than a formality.  At minimum: `set` with a quoted
symbol versus `setq` with a bare one; `set` on a symbol that has a lexical
binding in scope (**the case that decides the implementation** — do not
guess it, record it); `set` on an unbound symbol; `set`'s return value;
`set` with a non-symbol first argument; `set` with wrong arity.

Emacs' answer here is not obvious, and "obvious" is precisely what this
slice exists to replace with a recording.

### Numeric `=`

- arity: `(=)`, `(= 1)`, and the chained `(= 1 1 1)` / `(= 1 1 2)`;
- exceptional floats: `(= 0.0 -0.0)`, and NaN compared with itself;
- type errors: `(= 1 "1")`, `(= 1 nil)`;
- and the case that *documents the cut*: `(= x 3)` where `x` is unbound,
  which is an error after Phase 2 and was an assignment before it.

Phase 5 adds mixed integer/float cases; the parent plan is explicit that
Phase 5 **extends** this operation rather than redefining it, so write
these cases so an integer case slots in beside them.

## The condition-name trap, stated once

Phases 2 and 5 need `wrong-number-of-arguments` and `arith-error`, but the
condition system is **Phase 6**.  Until then these are *names carried in
`FeHandleError()`'s message*, not signalable symbols.

00B already encoded this: a fe-side condition record carries
`"condition_source": "message"` and the comparator treats it as a weaker
claim than the oracle's `"structured"`.  Use that field; do not strengthen
it.  **No case in this slice may assert a catchable condition object**, and
a case that wants to must be written as `planned` against Phase 6 instead.

## Gates

- Every construct in the table above has a manifest entry with the right
  status and, for `planned` entries, a named phase.
- Every case has a version-stamped oracle snapshot, regenerated
  reproducibly, and regeneration against a different Emacs build fails.
- `make -C fe compat` and `make check` are green **with the new cases
  reporting as known gaps**, not as failures — that is the mechanism
  working, and it is what lets 02B/02C flip statuses one at a time.
- `set`'s lexical-binding interaction is recorded from the oracle, and the
  recorded answer is written into the sub-plan as prose, so 02B implements
  against a stated rule rather than re-deriving it.
- No `.c` or `.h` file in either tree changes.  Complexity is unmoved in
  both trees; say so with measured numbers rather than assuming it.

## What this does not do

- It does not implement `setq`, `set` or numeric `=`.
- It does not delete or alter assignment `=`; that is 02C.
- It does not rename a single file; that is 02D.
- It does not add a case for anything Phase 5 will change the answer to.

## Status

Not started.
