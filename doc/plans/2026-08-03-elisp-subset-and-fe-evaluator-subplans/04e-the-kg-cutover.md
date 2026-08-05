# 04E — The kg cutover

Parent: [Phase 4](../2026-08-03-elisp-subset-and-fe-evaluator.md#8-phase-4--adopt-lisp-2-namespaces),
kg-only: the pin move plus every kg adaptation, in **one atomic green
commit**, per Rule 10.  This is Phase 4's 02D — the 81-file commit shape —
and it closes the phase.

**Prerequisite:** [04D](04d-the-namespace-cut.md), fully green in the
submodule including its nine-stage runner.

## Outcome

kg runs on Lisp-2 Fe.  The prelude's 53 definitions live in function
cells, the three head-position parameter calls are `funcall`, hooks and
process callbacks resolve designators through the function cell, the two
checkers that parse the prelude's spelling are updated with it, the 69
Lisp PTY cases pass unedited, and every document that described one
namespace tells the truth again.  A pin-only commit is impossible by
construction — `static_assert(FE_API_VERSION == 2)` fires — so the pin,
the prelude, the C adaptations, the tests and the docs move together.

## Files this slice owns

The `fe` gitlink; `lisp/prelude.el`, `lisp/auto-fill.el`,
`src/lisp_prelude_generated.inc` (regenerated); `src/lisp_core.c`
(version asserts), `src/lisp_hooks.c`, `src/lisp_process.c`,
`src/lisp_obj.c` (`functionp`); `test/test_lisp.c`,
`test/lisp-compat/features.json` + cases/snapshots,
`utils/check_lisp_compat.py`; `doc/lisp-api.md`, `doc/fe-upstream.md`,
root `README.md`, `doc/kg.1`; the set `README.md` (phase-closing Status).

## The prelude rewrite — the checklist the parent plan demanded

The parent's §8 asked for this as a checklist to check off, because a
missed site fails at *run* time in whichever command first uses `dolist`.
Verified against the current file (53 definitions, not the parent's stale
54; 18 macros, not 22):

1. **Every column-zero `(setq NAME ...)` becomes the 04A-pinned
   `(defalias 'NAME ...)` spelling** — 31 lambdas, 18 macros, and the
   4 primitive aliases (`progn`, `null`, `eq` via
   `(defalias 'progn (symbol-function 'do))` etc., since the alias must
   capture the primitive's function cell).
2. **`internal--let` survives**, contra the parent plan's own sentence
   (04A's Decision records the correction): it becomes
   `(defalias 'internal--let (symbol-function 'let))` **before** the Emacs
   `let` macro overwrites the primitive's function cell.  Ordering is
   still load-bearing; the header comment's rule 1 stands.
3. **The three head-position parameter calls become `funcall`**:
   `mapcar`'s `(f (car lst))`, `internal--dolist`'s `(body (car items))`,
   `internal--dotimes`'s `(body i)`.
4. **`let`'s two by-value passes become sharp-quotes**:
   `(mapcar #'internal--bind-name bindings)` /
   `(mapcar #'internal--bind-value bindings)`.  `let*` calls the same two
   helpers in head position and needs nothing — verify, don't assume.
5. **`(setq function (lambda (f) f))` is deleted** — the real special
   form replaced it in 04C/04D.  The count drops to 52; both parsers
   below move in step.
6. **`defun` retargets the function cell and fixes its command hand-off**:
   the expansion becomes `(defalias 'NAME f)` and the interactive branch
   passes `(function NAME)` (or `#'NAME`) to `define-command`, replacing
   the bare-symbol value read.  `defmacro` likewise via `defalias` of a
   `(macro ...)`.  `defvar`/`defconst`/`interactive` are value-namespace
   and stay `setq`-based inside their expansions.
7. **The 47 intra-prelude head-position uses of prelude-defined names**
   (`internal--let` ×17, `reverse` ×6, `eq` ×4, …) need no edit — they
   resolve through function cells once the definitions live there.  The
   quoted head symbols inside macro expansions (`internal--dolist`,
   `internal--save-excursion`, `define-command`, …) likewise.  This is
   the run-time surface the PTY suite exists to catch; trust the tests,
   not a grep.

`lisp/auto-fill.el` needs only what `defun` gives it; its
`(add-hook ... 'auto-fill--maybe-break t)` quoted-symbol idiom is the
hooks change's first consumer.

Then `make lisp-prelude-generate` and the `lisp-prelude-check` drift gate.

## The two parsers that pin the old spelling

Same commit, by necessity:

- `utils/check_lisp_compat.py`'s `parse_kg_prelude_defs` recognizes only
  `^\(setq NAME`; teach it the `defalias` spelling.  Re-derive, don't
  transliterate: the disjointness check (native names vs prelude names)
  and the exactly-one-`source_name` claim still hold; `function`'s claim
  moves from kg's manifest to fe's (the primitive now exists in
  `primitive_names[]`, and the kg prelude entry for the deleted identity
  lambda goes away).
- `test/test_lisp.c:test_prelude_source_file`: `PRELUDE_DEFS` 53 → 52,
  the shape scan follows the new spelling, the shape-to-`(type-of ...)`
  inference reads the *function* cell now (spell it as
  `(type-of (symbol-function 'NAME))`), and the `internal--let`-first
  ordering pin survives — it is the regression this test exists for.

## C-side adaptations

| Site | Change |
|---|---|
| `src/lisp_core.c` version asserts | `FE_API_VERSION == 3`, `FE_LANGUAGE_VERSION == 3` — both axes moved, unlike Phase 3. |
| `src/lisp_hooks.c:resolve_hook_function` | The only value-cell read in kg's C: `FeGetType(fn) == FeTSymbol ? FeEvaluate(ctx, fn) : fn` becomes the new `FeGetFunction` designator resolution.  Preserve the deferred-resolution header comment's contract — a symbol resolves when the hook *runs*, so redefinition takes effect, which `test_hooks` pins.  `remove-hook`'s root-identity matching is unaffected (symbols root as symbols). |
| `src/lisp_process.c:lisp_callable_or_nil` | Accepts symbols today but nothing ever resolves them — a latent bug.  Align with hooks: resolve via `FeGetFunction` at invocation, and add the one test that pins it. |
| `src/lisp_cmd.c` | No mechanical change: `define-command` receives an evaluated function object, and `defun`'s expansion now delivers it via `(function NAME)`.  The `"define-command requires a function"` rejection of raw symbols stays, with its test. |
| `src/lisp_obj.c` `functionp` | Learns designators per the pinned `(functionp 'car)` → `t` snapshot; `(functionp car)` in tests was reading value cells and is rewritten. |
| Native registration | No kg code change — `FeDefineNative` now targets function cells fe-side.  Consequence to assert, not assume: `(boundp 'insert)` is `nil` and `(fboundp 'insert)` is `t`. |

## Tests

- `test/test_lisp.c`, per the audited table: `test_type_predicates`
  (designator rewrite), `test_quasiquote` (`#'`/`function` now real —
  `(function car)` returns `car`, not `[primitive]`),
  `test_binding_forms` (`(mk 5)` → `(funcall mk 5)` — the closure-call
  case the parent plan predicted), `test_definition_forms`,
  `test_hooks` (both the value form and the quoted-designator form, plus
  redefinition-takes-effect), `test_void_function`/`test_void_variable`
  (the new message reality), `test_define_and_run_command`,
  `test_prelude_source_file` (above).  Add the coexistence case —
  `(setq f 7)` `(defun f () 9)` `(list f (f))` → `(7 9)` — as the
  phase's headline kg assertion.
- **The 69 Lisp PTY cases pass unedited.**  They are the run-time
  breakage net for every missed `funcall` and every hook/command path;
  an expectation edit there is a red flag to investigate, not a diff to
  accept.
- kg compat manifest: `one-namespace-clobber` → `supported` (kg now
  answers `(7 9)`, matching its snapshot); the `prelude-function` entry
  is deleted or re-pointed per the parser note above; entries for
  renamed/deleted prelude definitions move with their `source_name`s;
  `make lisp-compat-check` green is the structural proof.  Run
  `make lisp-compat-oracle` in verify mode; regenerate nothing.

## Documentation rewritten in the same commit

- `doc/fe-upstream.md`: the version tuple (`FeVersion` "4.0",
  `FE_API_VERSION` 3, `FE_LANGUAGE_VERSION` 3); the `#'`-as-identity
  divergence row **deleted or rewritten** — it documents a decision this
  phase reverses, and the "reintroducing exactly the silent wrongness"
  sentence in the deliberately-not-changed list goes with it; a new row
  for the Lisp-2 cut (symbol layout, `FeDefineNative`'s cell,
  `FeTMacro`-vs-`(macro . fn)` representation); the minimum-arena figure
  if 04C/04D moved it again.
- `doc/lisp-api.md`: the hooks contract (function-cell resolution), the
  quoting row (`#'f` is `(function f)` now), a namespaces section with
  the new forms (`funcall`, `apply`, `fboundp`, `symbol-function`,
  `symbol-value`, `fset`, `defalias`, `fmakunbound`), and the explicit
  differences list gains the honest residue (macro representation;
  message-level conditions until Phase 6).
- Root `README.md` (quoting row at the prelude table, the differences
  list, the `define-command` calling convention) and `doc/kg.1` (the
  prelude enumeration, the defun/interactive prose, the differences
  section).  `make docs-check` covers only key spellings; the prose is a
  named review item, per 03F's precedent.

## Gates

Phase-closing, Rule 9 in full: `make check` from an idle tree (32 native
suites; PTY total per the runner's own count — do not assert stale
literals), `make WITH_LISP=0 clean all check`, both complexity gates
(kg's Phase 4 row predicted +15 to +25 against 56 measured points of
headroom — record actuals and bank any credit), `header-check`,
`docs-check`, `lisp-compat-check`, `lisp-prelude-check`, and
`JOBS=8 .ci/run-ci-steps.sh --parallel` with a regenerated
`compile_commands.json` before believing `ci-06` — that trap has caught
this program twice.

Then close the phase: write the Phase 4 Status into the set README in the
established shape — what each slice cost against 04A's funded row, what
the plan got wrong, what is carried forward — and remove the five
sub-plan documents only after the reviewer accepts the completed
workstream.

## What this does not do

- **No `.fe`/value-cell fallback, no migration lint, no dual prelude.**
  §0.4; the cut is the cut.
- **No new kg natives** beyond the behaviour changes named — `funcall`
  and friends are fe-side.
- **No `internal--let` deletion** — Phase 8 Wave A, per 04A's recorded
  correction of the parent plan.
- **No interactive-argument work, no strict arity** — Phase 7 — and no
  condition objects — Phase 6.  `defun`'s `(interactive)` handling keeps
  its current strip-and-register shape, just through the function cell.
- **No PTY expectation edits and no oracle regeneration.**  Both are
  failure signals in this slice, not chores.
