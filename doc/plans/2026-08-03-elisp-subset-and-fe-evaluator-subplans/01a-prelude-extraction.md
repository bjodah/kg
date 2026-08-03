# 01A — Move the prelude out of C string literals

Parent: [Phase 1](../2026-08-03-elisp-subset-and-fe-evaluator.md#5-phase-1--move-the-kg-prelude-into-lisp-source).

**Prerequisites:** none technically.  Land after
[00C](00c-feature-inventory.md) so the inventory is written against the
file rather than against an array about to be deleted; if 00C slips, this
can go first.

## Why this exists, and why now

`src/lisp_prelude.c` holds 54 Lisp definitions as three C string literals —
split at 4095 bytes only because that is what C guarantees, with the split
points carrying no meaning.  Every line is `"...\n"` with escaped quotes.

Phase 2 rewrites every one of those 54 definitions from `=` to `setq`.
Phase 4 moves all 22 macros into the function namespace and inserts
`funcall` at three call sites.  Phase 5 changes what their numbers mean.
Doing any of that inside escaped C strings makes the diff unreadable at
exactly the moment review matters most.

This is also the last cheap moment.  The extraction is behaviour-neutral
today; after Phase 2 it is entangled with a semantic migration.

## Deliverables

```text
lisp/prelude.fe                 canonical source
utils/embed_lisp.py             generator
src/lisp_prelude_generated.inc  checked in; CI verifies no-diff regeneration
```

**`.fe`, not `.el`.**  The parent plan originally said `prelude.el`.  On
the day this file is created it contains 54 `(= name value)` forms — Emacs
Lisp's numeric comparison used as a statement.  `lisp/auto-fill.fe`
already establishes `.fe` for kg Lisp; both files rename together at the
Phase 5 flag day, when the claim becomes true.  One `git mv` then beats a
file that lies for four phases.

**The generator emits a byte array and an explicit length**, and must not
depend on NUL termination — the prelude is fed to
`FeEvaluateStringWithOptions(context, "prelude", text, len, &eval_options)`,
which already takes a length.  Checking the `.inc` in keeps ordinary builds
Python-free; `CLAUDE.md` records that the Makefile picks an interpreter
with `pexpect` and `PyYAML` and that `PYTHON` overrides it, so the
regeneration target uses the same resolution.

## Two test paths, not three

The parent plan asked for a third path: the prelude under standalone Fe
with kg-native stubs.  **Drop it.**  The prelude's later sections call
`string-length`, `string=`, `bounds-of-thing-at-point`,
`buffer-substring`, `internal--save-excursion` and
`internal--with-current-buffer`.  Stubbing those means writing a second
fake editor whose divergences from the real one become their own bug
source — to test a code generator.  `CLAUDE.md` already draws this line:
if behaviour depends on real cursor movement, windows or saved-file
output, it belongs in a PTY case, not a stubbed unit test.

What is left carries the whole property:

1. **The embedded generated representation** — the production path, which
   every existing Lisp test already exercises.
2. **`lisp/prelude.fe` loaded from disk** in a kg test, asserting the same
   54 definitions, each with the same type and arity.

Plus the CI check that regenerating produces no diff.  File, generator,
checked-in output: that triangle is what needs pinning.

Assert *definitions*, not a byte-identical evaluation trace.  The property
is that the source moved, not that the reader was re-derived.

## Ordering is load-bearing, and must stay so

`src/lisp_prelude.c`'s header records three rules.  Rule 1 survives and is
the one that can silently break during a mechanical move:

> An alias of a primitive must be taken before anything shadows that name
> (only `let` is shadowed), and a macro must not expand into a name that
> shadows what it meant.

Concretely: `(= internal--let let)` must precede the `let` macro that
shadows `let`, and every list-library function using `internal--let` must
follow both.  A generator that sorts, dedupes or reorders breaks the
prelude in a way no unit test that only checks *which* names exist will
catch.  Emit in source order, and add a test that the first definition is
`internal--let` and that the `let` macro follows it.

Rule 3 also survives: nothing in the prelude recurses over a list spine,
because Fe's GC stack caps recursion at a few hundred frames.  The one
deliberate exception is `equal`, which is iterative on the spine and
recursive only on the car.  Preserve the comment that says so.

## The stale comments, and the behaviour change one of them licenses

Rule 2 and one other claim are **false today**, and 00C's inventory will
have to record whichever version is true:

- **Rule 2 — "no macro may expand to bare nil" — is obsolete.**  It exists
  because Fe used to splice the expansion over the caller's cons cell and
  compare `nil` by address, so a macro expanding to `nil` produced a
  truthy nil-shaped object.  `fe.c:1863` no longer copies the expansion,
  and `doc/fe-upstream.md` lists the fix as a shipped divergence.  The
  four `(list 'quote nil)` workarounds — in `cond`, `setq`, `defvar` and
  `interactive` — can become plain `nil`.
- **"Macros also expand exactly once per call site" is false.**  They
  expand on *every* invocation, charged against the step budget.  That is
  now a performance statement, not a correctness one.  `README.md` and
  `doc/kg.1` have already dropped the old caveat; only this comment
  retains it.

**Removing the workarounds changes evaluated code.**  It is a separate
commit from the extraction, with its own test, and it is not covered by
this sub-plan's "behaviour-neutral" claim.  Land the move first, green;
then the cleanup, with a case showing `(cond)` and `(interactive)`
returning nil correctly without the quote.

If in doubt, leave the workarounds and only fix the *comments*.  They cost
nothing, and the four sites are rewritten again in Phase 2 anyway.

## Build and packaging

- `lisp/prelude.fe` is a source file, not a runtime dependency: the
  editor still evaluates the embedded copy, so a kg with no `lisp/`
  directory installed behaves identically.  Say this in the file's own
  header comment, or someone will "fix" the build by installing it.
- `WITH_LISP=0` must not gain a dependency on the generator or the file.
  `ci-08` proves it.
- `make clean`/`distclean` do not delete the checked-in `.inc`.
- The regeneration check joins `make check` beside `docs-check` and
  `header-check`, which are the existing structural no-drift checks.

## Gates

- The embedded and file-loaded preludes expose the same 54 definitions,
  with the same types and arities.
- Regenerating `src/lisp_prelude_generated.inc` produces no diff, and CI
  fails when it would.
- No hand-maintained Lisp source remains in C string literals.
- Definition *order* is preserved and tested.
- The three stale comments are corrected; any behaviour change they
  licensed lands in its own commit.
- `make check` (32 native suites, 405 PTY cases, 64 of them Lisp),
  `make WITH_LISP=0 clean all check`, `header-check`, `format-check`,
  `docs-check`, `coverage-check` all green.
- `make complexity-check` **and** `make pmccabe-check` green.  Expect this
  slice to *reduce* `src/lisp_prelude.c`'s scc — it measures 2 today, so
  the saving is small, but the file's 377 lines mostly leave.
- `.ci/coverage-baseline.json`: `src/lisp_prelude.c`'s line count changes
  substantially, so its rate will move.  Bank the new floor with `make
  coverage-baseline` and say in the commit message why the file shrank.

## What this does not do

- No semantic change to any definition.  `=` stays `=` until Phase 2.
- No new prelude definitions, however tempting the neighbouring gaps look.
- No rename of `lisp/auto-fill.fe`, and no `.el` anywhere.
- No load-path or init-file behaviour change; the prelude is not loaded
  from disk at runtime and this slice does not make it so.

## Status

Not started.
