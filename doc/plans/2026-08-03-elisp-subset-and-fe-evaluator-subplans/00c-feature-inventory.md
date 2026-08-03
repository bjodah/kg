# 00C — The feature inventory

Parent: [Phase 0](../2026-08-03-elisp-subset-and-fe-evaluator.md#4-phase-0--freeze-the-contract-and-establish-baselines),
kg work plus the corpus that fills 00B's schema.

**Prerequisites:** [00B](00b-oracle-and-differential-corpus.md) for the
record format and the manifest schema.  Drafting may start in parallel.

## Why this exists

The parent plan's gate is that "every existing kg Lisp primitive or
prelude construct appears in the feature inventory".  The exact size of
that, measured in this tree:

| Surface | Count | Where |
|---------|-------|-------|
| Fe primitives | 31 | `primitive_names[]`, `fe.c:64` |
| Fe primitive aliases | 1 | `fn` → `lambda` |
| kg natives | 78 | `native_bindings[]`, `src/lisp_prelude.c` |
| kg prelude definitions | 54 | the three string literals |
| Fe maths natives | several | in `fe.c` itself, so kg gets them |

163-ish entries.  That is a day of careful reading, not a sprint, and it
is the single highest-value artefact in Phase 0: it is what converts "does
kg support `mapconcat`?" from a grep into a lookup, and it is the list
every later phase's gate is checked against.

## Status values, and what each one obliges

The parent plan names four: `supported`, `planned`, `divergent`,
`unsupported`.  Tighten what each *costs*, or the manifest becomes
aspirational:

- **`supported`** — has at least one passing case appropriate to its
  comparison mode: an oracle case for `emacs`, a kg regression for
  `kg-policy`.  A `supported` entry with no case is a lie and the checker
  rejects it.
- **`planned`** — names the phase that delivers it.  A `planned` entry
  with no phase is a wish.
- **`divergent`** — has a case that *asserts the difference*, plus prose
  saying why.  This is the status that makes the manifest honest, and kg
  already has real instances to seed it with (below).
- **`unsupported`** — the construct must fail clearly.  An `unsupported`
  entry whose construct silently misbehaves is worse than no entry; the
  case asserts the clear failure.

`xfail: true` already exists in the PTY harness with `XPASS` failing
`make check`, and that is the right precedent: an expectation that
silently starts passing must break the build.

There is deliberately no `legacy` status.  The parent's §0.4 says the old
Fe/kg dialect has no compatibility constituency.  A current divergence can
become `planned` and then `supported`, or be deleted when its old spelling is
removed; inventorying it does not promise to keep it working.

## The taxonomy

The parent plan's ten categories are right; use them verbatim so the two
manifests share them: reader and literals; values and equality; bindings;
functions and macros; control flow; errors and non-local exits; sequences;
loading; interactive commands; editor primitives.

Ownership is `fe-core`, `fe-library` or `kg`, and it decides which
manifest the entry lives in.  Comparability is a separate field:
`comparison: emacs` or `comparison: kg-policy`.  A kg-owned prelude macro
can be pure and oracle-comparable; a kg editor native can implement an
intentional local policy.  Conflating those axes would leave `defun`, `let`
and the planned `defcustom` without an oracle merely because their source
file lives in kg.

Create `test/lisp-compat/cases/` and `test/lisp-compat/oracle/` beside the kg
manifest.  They use 00B's schema and generic Emacs runner.  A kg-owned
`comparison: emacs` case and its snapshot stay in kg; only a reference to a
Fe-owned prerequisite crosses the manifest boundary.

## Divergences to record on day one

These are not hypothetical; they are documented today and currently live
only in `doc/fe-upstream.md`'s prose table and in test comments.  Every one
becomes a `divergent` or `unsupported` entry with a case:

- `=` is assignment, not numeric comparison.  **The central one** — it is
  the reason the program exists, and it should be entry number one.
- `#'x` reads as plain `x`; `function` is a prelude identity lambda.
- One namespace: `(setq f 7)` then `(defun f () 9)` clobber each other.
- No integers.  `42` and `42.0` are the same object and print the same.
- `eq` is Fe's `is`; `equal` is a prelude lambda ending in `is`.
- Macros expand on every invocation, charged to the step budget.
- `?a` character literals are unsupported and must fail clearly — the
  reason is recorded in `doc/fe-upstream.md` and is exactly the "silent
  wrongness" this manifest exists to prevent: `FeReadFn` yields one byte
  at a time, so `?é` would read as the first UTF-8 byte.
- Strict arity is off: `((lambda (x) x))` is `nil`, not an error.
- An unassigned symbol raises `void-variable` — a divergence from *stock
  Fe*, and agreement with Emacs; the manifest should be able to express
  "differs from Fe upstream, matches the oracle".
- The writer is bounded: cyclic and deep structures print `#<cycle>`,
  `#<deep>`, `#<truncated>` where Emacs prints or hangs.
- kg's `load-path` is a bounded C array, not a settable list, so
  `add-to-load-path` exists where Emacs would `setq load-path`.
- Marker creation and exceptional floating-point formatting — the two the
  parent plan singles out as currently living only in test comments.

## Planned definitions to enter on day one

The inventory covers planned surface as well as current surface, so seed
`defcustom` now as `planned`, owned by `kg` in the prelude/library layer,
marked `comparison: emacs`, and delivered in Phase 8 Wave D.  Its cases must
distinguish:

- an unbound variable from an already bound one;
- whether the standard form is evaluated in each case;
- the returned symbol and stored docstring;
- accepted inert presentation metadata (`:type`, `:options`, `:group`,
  `:tag`, `:link`, `:version`, `:package-version`);
- rejected semantics-bearing and unknown keywords;
- the documented absence of dynamic binding, retained standard forms,
  `setopt`, and a Customize UI.

This entry prevents `defcustom` from becoming either a hand-waved synonym for
`defvar` or an accidental commitment to all of Custom.

## The kg manifest's extra obligations

Every kg entry names the native or PTY test that pins kg's result.  An
`emacs` entry also names the 00B oracle case whose snapshot supplies the
expected semantics; a `kg-policy` entry instead carries the rationale for
the divergence.  Most already have a kg test — 64 Lisp PTY cases and
`test/test_lisp.c` — and the inventory is the moment to find the natives
that have *none*.  Expect that list to be non-empty and treat it as this
sub-plan's second deliverable: a named set of untested natives, either
tested here or recorded as a gap with an owner.

`.ci/coverage-baseline.json` is the cross-check.  A native with no
reference in either harness will show as uncovered lines in its
`src/lisp_*.c` file's rate.

## Checker

One Python script under `utils/`, in the shape the other ratchet scripts
already have.  It asserts:

- schema validity of both manifests;
- no id collides between them;
- every `comparison: emacs` entry names an oracle case;
- every `comparison: kg-policy` entry names a kg test and rationale;
- every `supported` entry names a passing case;
- every `planned` entry names a phase;
- every `divergent` entry names a case and has prose;
- every entry's owner is one of the three values;
- and — the one that keeps it alive — **every Fe primitive, kg native and
  prelude definition present in the source appears in exactly one
  manifest.**  That last check is what stops the inventory rotting the
  first time somebody adds a native.

Wire it into `make check` next to `docs-check`, which is the closest
existing analogue: a dumb, structural check that a table and a document
have not drifted apart.

Also add `make lisp-compat-oracle` for kg's snapshots.  It uses 00B's Emacs
resolution/version rules and is a regeneration/verification target, not part
of ordinary `make check`; checked-in snapshots keep the normal suite
self-contained.

## Gates

- All 31 primitives, 78 natives and 54 prelude definitions have an entry.
- `defcustom` has the planned entry and subset cases specified above.
- The checker fails when a native is added without one.  Prove it by
  adding one temporarily.
- Every `divergent` entry has a case asserting the difference.
- Every kg-owned `comparison: emacs` entry has a kg-local case and
  version-stamped oracle snapshot.
- The list of natives with no test exists and each is either tested or
  recorded as a gap with an owner.
- `make check` and `make WITH_LISP=0 clean all check` green; both
  complexity gates green in both trees.

## What this does not do

- It does not implement or fix anything.  A native discovered to be
  broken during the inventory gets an entry and a bug note, not a fix in
  this slice.
- It does not enumerate Emacs.  The manifest lists what kg has and what
  the program plans; "everything Emacs can do" is not a finite list and
  the parent plan's §2 explicitly disclaims it.

## Status

Not started.
