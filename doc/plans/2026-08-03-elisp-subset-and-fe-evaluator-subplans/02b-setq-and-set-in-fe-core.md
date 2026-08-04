# 02B — `setq` as a core special form, `set` as a function

Parent: [Phase 2](../2026-08-03-elisp-subset-and-fe-evaluator.md#6-phase-2--hard-cut-assignment-and-numeric-),
Fe changes.

**Prerequisites:** [02A](02a-pin-the-target-semantics.md).  Its recorded
oracle answers — particularly `set`'s interaction with lexical bindings —
are this slice's specification.

## Why this is smaller than it looks

The parent plan makes the point and it is worth not losing:

> `EvaluatePrimitive`'s existing `PSet` arm already *is* single-pair
> `setq`: `CDR(GetBound(ctx, va, env)) = EVAL_ARG()` updates a lexical
> binding when one exists and the global cell otherwise.  What Phase 2
> adds is pair iteration, the returned value, and the arity diagnostic.
> **Do not design a new binding path.**

Verified in this tree: `=` is `PSet` (`fe.c`'s `primitive_names[]`,
`[PSet] = "="`), and `GetBound` walks the lexical environment first and
falls back to the symbol's global cell.  So `setq`'s semantics already
exist and are already tested; what is missing is a loop, a return value
and an error.

The temptation this slice must resist is treating "add `setq`" as licence
to restructure binding.  A new binding path would be a Phase 3-sized
change smuggled into a Phase 2 slice, and it would land *before* the frame
machine that is supposed to reshape evaluation.

## Ordering inside Phase 2

`setq` arrives here; assignment `=` **survives this slice**.  That is
deliberate and is not a compatibility promise: it is an intra-repository
sequencing choice, so that fe's own 99 tracked `(= ...)` assignment sites
keep working while `setq` is proven, and 02C can migrate them in one
reviewable commit against a `setq` that already exists.

Both spellings coexist for exactly two slices, inside one repository, with
no release between them.  Parent §0.4 forbids compatibility *aliases for
users*; it does not forbid landing a replacement before deleting what it
replaces.  Say this in the commit message, because the diff alone looks
like the thing §0.4 prohibits.

## `setq`

A core special form, because it does not evaluate its symbol arguments.
Behaviour is 02A's recorded case list, not this document's memory of it.
The shape:

- pair iteration over the existing single-pair `PSet` mechanism;
- values evaluated left to right, each assignment visible to the next;
- odd argument count raises `wrong-number-of-arguments` **as a message**,
  not as a catchable condition (Phase 6 owns that; see 02A);
- zero arguments returns `nil`; otherwise the last assigned value.

## `set`

An ordinary function — it evaluates its first argument.

**Implement it against 02A's recorded answer, not by aliasing `setq`.**
The parent plan says so explicitly, and the reason is that the two differ
precisely where it is easy to be wrong: what `set` does to a symbol that
has a lexical binding in scope. If 02A's recording shows Emacs' `set`
ignores the lexical binding and writes the global cell, then `set` is
*not* `setq` with an evaluated argument, and the implementation must not
be able to drift into becoming that.

Add a case that would fail if someone later "simplified" `set` into a
`setq` alias.

## Budget

00A's price table, fe row, Phase 2: **+20 to +30**, net of deleting the
assignment arm — and that deletion is 02C's, not this slice's.  So this
slice spends and 02C refunds.

fe entered Phase 2 at **214 of 220** (`fe.c` 106 of 112).  A `setq` arm
plus a `set` native plausibly exceeds the 6 points of headroom before 02C
gives any back.

**This slice therefore needs its own dated Decision**, in the format 00A
established — date, trigger, number, funding or its explicit absence —
recorded in this directory's README.  It has a genuinely named funding
source, which is unusual and worth stating plainly: 02C deletes the `PSet`
assignment arm and 02D deletes kg's prelude `setq` macro.  This is a loan
against work that is *already planned and scheduled*, not against a hope.

Follow Rule 10: the cap change lands in the fe submodule, kg's pin moves
in a separate green kg commit.

Also re-read 00A's finding before assuming file-cap headroom is real:
scc's C parser desynchronises on `fe.c`'s `'"'` character literals, so
`fe.c`'s 106 is a **floor**, not a measurement.  Run `make -C fe
complexity-check` *and* `make -C fe pmccabe-check` at the start and the
end; pmccabe is the trustworthy per-function number.

## Gates

- 02A's `setq` and `set` cases pass against fe, and their manifest
  entries move `planned` → `supported`.
- The `set`-is-not-`setq` case exists and would fail on an alias.
- Assignment `=` still works; fe's 19 `scripts/*.fe` and `test_api.c` are
  untouched and green, including the third pass under `fe -a`.
- No case asserts a catchable condition object.
- Both complexity gates green in both trees, at whatever number this
  slice's Decision states.
- `make -C fe check` green; kg's `make check` and `make WITH_LISP=0 clean
  all check` green after the pin move.

## What this does not do

- It does not remove assignment `=` or bind `=` to comparison (02C).
- It does not touch `FE_LANGUAGE_VERSION`, which does not exist yet (02C
  introduces it).
- It does not migrate fe's scripts, kg's prelude, or any filename.
- It does not redesign binding, environments or `GetBound`.

## Status

Not started.
