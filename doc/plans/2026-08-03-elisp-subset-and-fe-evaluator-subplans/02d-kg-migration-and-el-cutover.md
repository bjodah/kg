# 02D — kg's migration to `setq`, and the `.el` dialect cutover

Parent: [Phase 2](../2026-08-03-elisp-subset-and-fe-evaluator.md#6-phase-2--hard-cut-assignment-and-numeric-),
kg migration and the dialect/filename cutover.

**Prerequisites:** [02C](02c-the-equals-hard-cut-in-fe.md).  kg cannot move
its pin until fe's cut has landed and passed in the submodule (Rule 10).

## Why this is one slice and not two

The parent plan puts the filename cutover **in the same commit** as the
pin move, and that is not an aesthetic preference.  The moment kg's pin
moves to a fe where `=` is comparison, kg's prelude — 54 `(= name value)`
forms — stops being a program and becomes 54 comparisons whose results are
discarded.  There is no intermediate state where kg builds and works.  So
the pin move, the prelude rewrite, the rename and the loader change are
one atomic kg commit, and Rule 10's "a deliberate API/language break must
not create a pin-only commit that cannot build" is exactly this case.

That makes this the largest single commit in Phase 2.  It is worth
staging the work locally in pieces and squashing, rather than pretending
it can land incrementally.

## The measured surface

Every site below was located in this tree.  Symbols are authoritative;
line numbers will drift.

**Lisp source (the assignment migration):**

| File | `(= ...)` sites |
|---|---|
| `lisp/prelude.fe` | **54** |
| `lisp/auto-fill.fe` | **0** — already `defvar`/`defun`/`setq` |

`auto-fill.fe` needs the *rename* but not the migration, which is a small
piece of good news: 01A's decision to keep it on `.fe` cost nothing.

**The prelude's `setq` macro is deleted here.**  It exists only to expand
into assignment `=`; once `setq` is a core special form (02B), the macro
shadows a better implementation with a worse one.  Deleting it also
deletes one of the four `(list 'quote nil)` workarounds 01A deliberately
left in place.

**C sites that spell `.fe`:**

- `src/lisp_core.c` — `lisp_config_path(..., "init.fe")`, the init-file
  discovery path.
- `src/lisp_require.c` — the `"%s/%s.fe"` candidate builder for bare-name
  package resolution, plus two comments that spell the pattern.
- `src/lisp_io.c` — the `"lisp/%s.fe"` stem and its `sizeof("lisp/.fe")`
  bound.  **Note the sizeof**: `.el` and `.fe` are the same length, so this
  bound does not change size, which is exactly the kind of thing that
  silently stays right and should be checked rather than assumed.
- `src/syntax.c` — `LISP_HL_extensions[] = { ".fe", ".lisp", ".lsp" }`.

**Everything else:** `Makefile`'s `lisp-prelude-generate` /
`lisp-prelude-check` targets (four spellings of `lisp/prelude.fe`),
`utils/embed_lisp.py`'s docstring, `utils/check_lisp_compat.py`'s
`LISP_PRELUDE_FE`, `test/test_lisp.c`'s `test_prelude_source_file` (which
opens the file by name), `src/lisp_prelude.c`'s header comment, **52 PTY
YAML cases** that reference `.fe`, `README.md`, `doc/kg.1`,
`doc/lisp-api.md`, and `doc/TODO.md`.

## `syntax.c` is the one that is not a rename

Adding `.el` to `LISP_HL_extensions[]` is **user-visible behaviour**: it
changes which files get Lisp highlighting.  Rule 7 and `make docs-check`
apply — `README.md`, `doc/kg.1` and, if any binding or help text mentions
it, `src/help.c`.

Decide and state whether `.fe` stays in that list.  The honest answer is
that it should **go**: §0.4 says there are no user files to service, and a
highlighting entry for a dialect kg no longer speaks is exactly the "dead
migration infrastructure" the parent plan warns against.  If it stays,
that is a decision with a reason, not an oversight.

## Hard cut means hard cut

The parent plan enumerates this and it should not be softened:

- rename `lisp/prelude.fe` → `lisp/prelude.el` and
  `lisp/auto-fill.fe` → `lisp/auto-fill.el`;
- startup loads only `<config>/kg/init.el`;
- bare package names resolve to `.el`;
- rename every tracked init/package fixture and update user documentation.

**No `init.fe` fallback, no `.fe` bare-name fallback, no warning, no
deprecation window.**  A literal path passed to `load` stays literal — the
cutover changes *discovery and bare-name resolution*, not the ability to
open a file somebody names explicitly.  Do not "helpfully" try `.fe` when
`.el` is missing; that is the fallback, wearing a different hat.

## Ordering trap inherited from 01A

`lisp/prelude.fe`'s ordering is load-bearing, and `test_prelude_source_file`
asserts it through its consequence: the first definition must be
`internal--let` and it must answer `primitive`.  That test opens the file
**by name**, so it breaks at the rename and must be updated in the same
commit.

More importantly: the migration rewrites all 54 definitions.  Rule 1 —
"an alias of a primitive must be taken before anything shadows that name"
— is exactly as load-bearing after the rewrite as before, and a
search-and-replace that reorders nothing is fine, while any tidying that
reorders is not.  The test is the guard; do not weaken it to make the
rename pass.

## Version agreement

02C introduced `FE_LANGUAGE_VERSION`.  kg asserts the version it requires
at compile time, the way `src/lisp_core.c` already does
`static_assert(FE_API_VERSION == 1)`.  That assertion is what makes a
half-applied cutover a build failure instead of a runtime mystery, so it
lands in the same commit as the pin move.

## Budget

00A's kg row for Phase 2: **−2 to +5**, likely self-funding, priced
against `src/lisp_require.c`.  Deleting the prelude's `setq` macro and one
`(list 'quote nil)` workaround should pay for the loader's extension
check.

kg entered Phase 2 at **5443 of 5500**.  This row is not the risk; if it
overruns, something has been built that this slice did not intend to
build, and the right response is to stop and report rather than spend
Phase 3's allowance.

If 02B raised fe's cap against 02D's macro deletion as named funding,
report the measured repayment here.

## Gates

- No production Lisp in either repository uses `=` for assignment.
- The prelude's `setq` macro is gone, **and its manifest entry with it** —
  `utils/check_lisp_compat.py` fails otherwise, which is the intended
  coupling.
- `lisp/prelude.el` and `lisp/auto-fill.el` exist; no `.fe` remains under
  `lisp/`.
- Startup discovers only `init.el`; bare names resolve only to `.el`;
  a literal `.fe` path passed to `load` still opens.
- `make lisp-prelude-check` still proves the file and the generated `.inc`
  agree, through the rename.
- `test_prelude_source_file` still passes, still asserts the order, and
  still asserts `internal--let` answers `primitive`.
- 02A's kg-side cases pass; statuses updated; deleted constructs' entries
  deleted.
- `README.md`, `doc/kg.1`, `doc/lisp-api.md`, `doc/TODO.md` and the 52 PTY
  cases are updated; `make docs-check` green.
- `make check` and `make WITH_LISP=0 clean all check` green; both
  complexity gates green in both trees.
- Phase 2 ends here, so it ends with `.ci/run-ci-steps.sh --parallel`
  (Rule 9), from an **idle tree** — a concurrent build has already
  produced one false reading in this program.

## What this does not do

- It does not change what `=` means; 02C did that.
- It does not add integers, a condition system, or strict arity.
- It does not rename `.fe` anywhere outside kg's own `lisp/` and fixtures.
  fe's `scripts/*.fe` are fe's own dialect artefacts and stay as they are.

## Status

Not started.
