# 02C — Delete assignment `=`, bind `=` to numeric equality, version the language

Parent: [Phase 2](../2026-08-03-elisp-subset-and-fe-evaluator.md#6-phase-2--hard-cut-assignment-and-numeric-),
Fe changes.

**Prerequisites:** [02B](02b-setq-and-set-in-fe-core.md).  `setq` must
exist before the 99 assignment sites can move to it.

## Why this exists

This is the slice the program is named for.  `=` stops being assignment
and becomes numeric comparison, in one cut, with no alias left behind.

## The measured migration surface

Counted in this tree, not estimated:

| Where | `(= ...)` sites |
|---|---|
| `fe/scripts/*.fe` and `fe/tests/*` | **99** |
| `lisp/prelude.fe` (kg) | 54 |
| `lisp/auto-fill.fe` (kg) | **0** — already `defvar`/`setq`/`defun` |

kg's 54 are 02D's.  This slice owns fe's 99, and they are the reason 02B
lands `setq` first: migrating 99 call sites against a `setq` that does not
yet exist would mean one commit that both adds the form and rewrites every
user of the old one.

## Order within the slice

Three commits, in this order, each green:

1. **Migrate fe's tracked scripts and tests** from `(= x v)` to
   `(setq x v)`.  Pure mechanical Lisp; assignment `=` still works, so
   this commit is behaviour-neutral and reviewable on its own.
2. **The cut.**  Remove the `PSet` assignment arm and the `=` → `PSet`
   binding; bind `=` to a numeric-equality arm shaped like the existing
   `PLess`/`PLessEqual` arms.  02A's recorded cases are the specification.
3. **Version the language.**  Introduce `FE_LANGUAGE_VERSION` (there is no
   such constant today; `FE_API_VERSION` is 1, in `fe/fe.h`, asserted at
   compile time by `src/lisp_core.c`).

Commit 2 is the one that cannot be split further, and it is where a
reviewer should be pointed.

## The migration audit, and the checker not to build

The parent plan is precise about this and the reasoning is worth keeping:

> Do not add a permanent syntax-shaped linter: after Phase 2,
> `(= SYMBOL VALUE)` can be a legitimate numeric comparison, so such a
> checker either rejects valid code or becomes dead migration
> infrastructure.

So: a **one-time audit** across tracked fe scripts, fe tests, kg Lisp,
fixtures and documentation, run before commit 2, its result recorded in
the commit message. Not a `utils/` script, not a CI step, not a ratchet.

There is also no untracked-user-file hazard to service (§0.4).  The full
tracked suites in both repositories plus 02A's differential cases are the
coverage.

## `FE_LANGUAGE_VERSION`

Introduce it *because the cut is invisible otherwise*.  `FE_API_VERSION`
tracks the C embedding contract, which this slice does not change; a kg
built against pre-cut fe would compile fine and then misbehave at runtime,
which is exactly the failure mode a version constant exists to prevent.

- Define it in `fe/fe.h` beside `FE_API_VERSION`.
- kg asserts the version it requires at compile time, the way
  `src/lisp_core.c:43` already does `static_assert(FE_API_VERSION == 1)`.
- Phase 5 bumps it again when integers change the numeric contract; write
  it so that bump is a one-line change.

The version moves in the **fe** commit; kg's matching assertion moves in
02D's pin commit.  That is Rule 10, and it means the two repositories are
briefly out of step in the tree — which is exactly what the assertion is
for, and why 02D is not optional.

## Budget

00A's fe row for Phase 2 is **+20 to +30 net of deleting the assignment
arm**, and this is the slice that performs the deletion.  Expect it to
give back part of what 02B borrowed.  Record scc and pmccabe before and
after in both trees and, if a durable saving lands, **bank it** with fe's
`make complexity-baseline` (Rule 6).

If 02B raised a cap against this slice as its named funding, say in this
slice's commit message how much came back, and lower the cap if the
measurement supports it.  A loan whose repayment is never checked is a
raise.

## Gates

- 02A's numeric-`=` cases pass; their manifest entries move `planned` →
  `supported`.
- The `=`-as-assignment entry is **deleted** from the manifest, not
  demoted — 00C's inventory has no `legacy` status, deliberately.
- No assignment behaviour is reachable through `=`, an alias, or any
  remaining primitive.  Prove it: `(= x 3)` on an unbound `x` errors.
- fe's 19 `scripts/*.fe`, its `tests/*.out`/`*.err` expectations, and
  `test_api.c` are all migrated and green, **including the third pass
  under `fe -a`** (strict arity), which is where an arity mistake in the
  new `=` arm will surface.
- `FE_LANGUAGE_VERSION` exists and kg asserts it.
- The one-time audit was run and its result is in the commit message.
- No permanent assignment-`=` linter was added.
- Both complexity gates green in both trees; the net delta against 00A's
  Phase 2 fe row is stated as a measured number.
- `make -C fe check` and fe's full `.ci/run-ci-steps.sh` green.

## What this does not do

- It does not touch kg's prelude, kg's filenames, or kg's docs (02D).
- It does not add integer support; `=` compares the existing doubles.
  Phase 5 extends it to mixed types and must not have to redefine it.
- It does not build a deprecation path, a warning, or a transitional
  release.  §0.4: there is nobody to warn.

## Status

Not started.
