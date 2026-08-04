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

**Complete, 2026-08-04.** Both manifests are populated and cross-checked.

**Fe's half** (`fe/compat/features.json`, fe submodule, `analyzers-etc`
branch): grew from 00B's 5 proof-of-mechanism entries to 44 -- the 31
primitives + 1 alias (`fn`) this slice's gate requires, plus 12 more
fe-owned cross-cutting divergences named in the parent sub-plan's
"Divergences to record on day one" list that do not belong to kg
(`#'x`-as-identity, no integers, `?a` character literals, lax arity, the
already-agreeing void-variable case, the bounded writer, and the
one-namespace `boundp` demonstration). Status breakdown: 30 `supported`,
11 `divergent`, 2 `unsupported`, 1 `planned` (the pre-existing
`signal-and-quit`); comparison: 38 `emacs` (each with a checked-in,
version-stamped snapshot against GNU Emacs 31.0.90), 6 `kg-policy` (raw
Fe primitives with no Emacs-nameable analogue -- `env`, `do`, `is`, the
anonymous-macro-call form, the historical `fn` alias, and `print`, whose
own side-channel output collides with both runners' single-line stdout
protocol). One real mechanism gap found and fixed along the way:
`fe/utils/run-fe-compat.py` had no branch for `comparison: kg-policy` and
demanded an oracle snapshot unconditionally; it now skips the oracle
lookup for that comparison mode and reports the case as pinned by its
`kg_test` instead (fe commit, `analyzers-etc` branch, ahead of kg's pin
at the time of writing -- see the commit list below).

**kg's half** (`test/lisp-compat/features.json`, new): 136 entries -- the
78 natives (`native_bindings[]`) and 54 prelude definitions
(`lisp_prelude[]`'s three string literals) this slice's gate requires,
plus the `defcustom` planned entry and three kg-owned cross-cutting
divergences (`one-namespace-clobber`, the compound `(setq f 7) (defun f
() 9)` example the parent plan names directly; `macro-expand-every-invocation`;
and `format-exceptional-float`, the second of the two "living only in
test comments" items the parent plan calls out, `make-marker`'s
detached-vs-at-point divergence being the first). Status breakdown: 125
`supported`, 7 `divergent`, 3 `unsupported` (the untested-natives gaps,
below), 1 `planned` (`defcustom`); comparison: 53 `emacs` (every one with
a real, version-stamped Emacs snapshot generated and verified against
`/opt-3/emacs-31-lucid/bin/emacs` during this slice, not carried over
from a prior claim), 83 `kg-policy`. Two divergent entries were found
empirically, not from the parent plan's list, by actually running cases
against the pinned oracle: `native-string-to-char` (`(string-to-char "")`
is `nil` in kg, `0` in Emacs) and `native-type-of` (kg's vocabulary is
its own object model, not Emacs' type lattice -- `(type-of 1)` is
`"double"` vs `'integer`).

**Divergence verification method.** Every `comparison: emacs` case in
both manifests was run for real against `/opt-3/emacs-31-lucid/bin/emacs`
via `fe/utils/run-emacs-oracle.py` (kg's `test/lisp-compat/` pointed at
the same generic runner 00B built, per its own design intent) and, for
kg's 54 prelude definitions specifically, cross-checked by extracting the
exact `lisp_prelude[]` source out of `src/lisp_prelude.c` (stripping C
comments first -- an early extraction attempt without that step
corrupted two definitions by picking up quoted Lisp fragments from inside
comments) and evaluating it under the standalone `fe` binary with a
5-line stub for the four kg natives the prelude calls
(`string-length`/`string=`/`buffer-substring`/`bounds-of-thing-at-point`)
side by side with the same expressions in real Emacs. Every one of the 35
prelude forms initially expected to agree did, including `quasiquote`'s
full backquote/unquote/unquote-splicing behavior and, notably, kg's
`setq` macro's multi-pair/return-value semantics -- built on Fe's raw `=`
assignment primitive (itself the central, entry-number-one divergence)
but already matching Emacs' `setq` contract today. This is the manifest
expressing exactly the nuance the parent plan asked for: two independent
axes, one underlying primitive.

**Untested natives** (the second deliverable, all recorded `unsupported`
in kg's manifest with an explanatory rationale and an owner rather than
fixed): `buffer-list`, `re-search-backward`, `process-status`. A fourth
apparent gap, `buffer-list`, was initially suggested by a naive text
sweep of `test/pty/*.yaml` but traced to an unrelated match (the
`"buffer-list"` *keymap* name in `describe-key-shadowed-mode-binding.yaml`,
not the Lisp native) -- corrected before being recorded. Two natives that
looked untested by name (`internal--save-excursion`,
`internal--with-current-buffer`) and eleven private prelude helpers
(`internal--append2`, `internal--first`, `internal--bind-name`,
`internal--bind-value`, `internal--dolist`, `internal--dotimes`,
`internal--qq`, `internal--qq-list`, `internal--interactive-p`,
`internal--has-interactive`, `internal--strip-interactive`) are recorded
`supported`, tested-via-wrapper: each is a thin private implementation
detail exercised on every call its public macro's own test makes
(`save-excursion`, `with-current-buffer`, `append`, `prog1`, `let`/`let*`,
`dolist`, `dotimes`, `quasiquote`, `defun`'s interactive handling), which
this slice's rationale field says explicitly rather than leaving as a
silent assumption. No native was found broken.

**The checker** (`utils/check_lisp_compat.py`, wired into `make check`
next to `docs-check`, plus `make lisp-compat-oracle` as a
regeneration/verification target using `fe/utils/run-emacs-oracle.py`
directly, restricted to `comparison: emacs` cases so it does not litter
`kg-policy` snapshot files): reuses `fe/utils/check_compat_manifest.py`
for both manifests' schema/per-entry validation and their id-collision
check (via `--other-manifest`, both directions) rather than
reimplementing it, then adds the source-coverage check by parsing
`fe/fe.c`'s `primitive_names[]`/`primitive_aliases[]` and
`src/lisp_prelude.c`'s `native_bindings[]`/`lisp_prelude[]` directly and
requiring every one of the resulting 32 + 78 + 54 source names to be
claimed by exactly one feature entry's `source_name` field, plus the
`planned`-names-a-phase rule and the `defcustom`-shape check 00B's own
checker did not need. One real subtlety the checker had to get right:
`let` is a legitimate name collision, not a duplicate claim -- Fe's raw
single-binding `let` primitive (`fe/compat/features.json`'s
`primitive-let`, `divergent`) and kg's Emacs-shaped multi-binding `let`
macro that shadows it (`test/lisp-compat/features.json`'s `prelude-let`,
`supported`) are two different source declarations that happen to share a
spelling; the coverage check verifies each of the three source pools
(fe primitives, kg natives, kg prelude definitions) independently rather
than flattening them into one global name set, which would have wrongly
flagged this as a double claim. Proven to fail on an unlisted native: a
throwaway `{ "totally-new-native-xyz", native_message }` row added to
`native_bindings[]` produced `FAIL: kg native or prelude definition
'totally-new-native-xyz' has no feature entry naming it as source_name`;
reverted (`diff` against a saved copy confirmed a byte-identical revert)
before committing.

**Not done, deliberately, per scope discipline:** no language behavior
changed; three untested natives recorded as gaps rather than given new
tests; `run-fe-compat.py`'s kg-policy gap was fixed (it blocked
`make -C fe compat` outright, not a "native discovered broken" case, and
its own submodule's standalone CI needed to stay green) but no other
existing behavior was touched. One pre-existing, unrelated finding
surfaced by running `.ci/run-ci-steps.sh --parallel` in kg: `ci-06`
(IWYU/clang-tidy/cppcheck) fails on `src/lisp_core.c`'s unused
`#include <time.h>`, introduced by the prior 00D slice's commit `bfa9d7e`
(`git log` confirms; `git diff` confirms this slice touches no `.c`/`.h`
files at all) and not part of 00C's scope -- reported, not fixed.

Full verification, complexity, and commit details are in 00C's parent
report; complexity is unchanged in both trees (kg 5444/5500, fe 214/220
with `fe.c` 106/112, both pmccabe baselines 0 new/gone/improved) because
this slice added no `.c`/`.h` files to either tree except the one-line
`run-fe-compat.py` fix, which is Python.
