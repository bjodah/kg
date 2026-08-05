# 04D — The namespace cut

Parent: [Phase 4](../2026-08-03-elisp-subset-and-fe-evaluator.md#8-phase-4--adopt-lisp-2-namespaces),
fe-only — **and deliberately without a kg pin move.**

**Prerequisite:** [04C](04c-the-function-namespace-additively.md).  The
machinery must be complete and tested before the fallback that hides it is
deleted.

## Outcome

Fe is a Lisp-2.  A symbol in call position resolves through its function
cell only; the value-cell fallback is deleted; `#'x` reads as
`(function x)`; the bootstrap's callables live in function cells; fe's own
scripts are migrated; and the language and API versions say so.  This is
the 02C precedent — the hard cut lands in the owning repository, passes
its full standalone CI, and kg adapts in the next slice's separate green
commit.  **kg's pin does not move here**: a pin-only commit cannot work
(kg's prelude would break at startup and
`static_assert(FE_API_VERSION == 3)` has no kg-side edit yet), which is
exactly the state Rule 10's "no pin-only commit that cannot build" clause
anticipates.  04E is the pin.

## Files this slice owns

`fe/fe.c` (bootstrap, reader, writer), `fe/fe_eval.c` (head resolution,
fallback deletion, `HandleNonCallable`/design-comment rewrite),
`fe/fe.h` (`FE_API_VERSION` 2 → 3, `FE_LANGUAGE_VERSION` 2 → 3,
`FeVersion` "3.0" → "4.0", `FeDefineNative`'s contract comment),
`fe/test_header.c`, `fe/example_host.c`, `fe/test_api.c`,
`fe/scripts/*.fe` and `fe/tests/*` where output legitimately changed,
`fe/compat/features.json` + the two flipped snapshots' entries,
`fe/fuzz/fe.dict` and the eval-fuzz grammar, `fe/README.md`,
`fe/doc/language.md`, `fe/doc/c-api.md`, `fe/doc/implementation.md`.

## The cut, site by site

1. **Bootstrap moves to function cells.**  The primitives, the `fn`
   alias, and the 16 math natives registered through `FeDefineNative` all
   land in function cells; `t`, `pi` and `e` stay values.
   `FeDefineNative` itself now writes the function cell — the one meaning
   change in the public API, carried by the version bump.  The alias
   registration reads the canonical symbol's *function* cell to share the
   object.
2. **Head resolution loses the fallback.**  04C's helper resolves the
   function cell (with designator chains) and, on unbound, reports
   `void-function NAME` — now a fact, not the fiction the old design
   comment described; rewrite that comment.  A lexical binding no longer
   shadows call position: `(let ((car 5)) (car x))` works, per the pinned
   snapshot.  Variable reference is untouched.
3. **`#'` reads as `(function x)`.**  Replace the identity branch in
   `Read`'s `'#'` case with the `ReadWrapped` construction every other
   reader macro uses; the `stray '#''` diagnostic survives.  `#` remains
   an ordinary symbol character otherwise.
4. **The writer prints `(function X)` as `#'X`** if — and exactly as —
   04A's Decision recorded from the `(quote x)` precedent, so the pinned
   `reader-sharp-quote-identity` snapshot (`printed: "#'car"`) passes as a
   comparison rather than being rewritten.  If 04A found fe does not
   abbreviate `quote` either, the Decision said what to do; follow it, do
   not decide here.
5. **Macros resolve through the function cell** with no code change —
   dispatch already types whatever resolution returned; only where
   definitions live moved.  fe keeps `FeTMacro` as its representation;
   the `kg-policy` divergence entry from 04A covers the
   `symbol-function`-of-a-macro difference.

## Migrating fe's own scripts

Every tracked script that defines a callable does it with
`(setq name (fn ...))` or `(setq name (macro ...))` — value cells.  Audit
all 28 scripts (02C's method: read them, do not just grep — 02C's
migration famously caught `scripts/macros.fe` building expansions
*programmatically*, and the same file is in this slice's path):

- definitions become `(fset 'name (fn ...))` / `(fset 'name (macro ...))`
  or `defalias`, per the spelling 04A pinned;
- macro *expansions* that assign variables (`++`'s `(list 'setq sym ...)`)
  are value-namespace and stay `setq`;
- a lambda in head position stays legal and stays as-is;
- `scripts/assert.fe` migrates first since every case preloads it.

Goldens: regenerate **only** the bytes that legitimately changed —
error-message texts (`void-function` where `void-variable` was, the new
`stray`/unsupported-function-form spellings) and any printed `#'`
abbreviation.  Everything else must remain byte-identical, including all
nine 03A `frame-trace-*.fe` traces (they contain pair forms, not lookup
internals — if a trace changes, that is a finding about frame semantics,
not a golden to refresh).  The `-a` strict-arity pass must stay
byte-identical with the other two, per `test.sh`'s rule.

## Versions, and what each one claims

- `FE_LANGUAGE_VERSION` **2 → 3**: call-position resolution, `#'`, and
  `boundp`-of-a-callable all changed meaning — the same axis Phase 2's
  `setq`/`=` cut moved.
- `FE_API_VERSION` **2 → 3**: `FeDefineNative` changed meaning, and
  `FeSetFunction`/`FeGetFunction`/`FeIsFBound` joined the surface in 04C
  under the old number — the bump makes the whole namespace contract one
  visible break.  kg's `static_assert(FE_API_VERSION == 2)` firing on the
  next pin move is the designed tripwire; `fe/test_header.c` and
  `fe/example_host.c` move here so fe proves its own header.
- `FeVersion` **"3.0" → "4.0"**.

## Compat flips — this slice's evidence

- `one-namespace-boundp` → `supported`: `(boundp 'car)` is `nil`, matching
  the snapshot that has been recording the target since 00C.  Rewrite the
  rationale (it currently *explains the divergence*; now it names the cut).
- `reader-sharp-quote-identity` → `supported`, and fix its stale
  `fe.c:1411` reference while touching it.
- Audit every other entry whose case involves calling or quoting symbols
  (`unbound-symbol-void-variable`, `primitive-setq`, `primitive-set`,
  `primitive-boundp`, `primitive-makunbound`, the alias entry): check,
  do not assume, that their comparisons still pass; a moved answer is
  either a bug or a manifest finding with an oracle case.
- `make -C fe compat` green is the flip's proof; no snapshot regenerated.

## Fuzzing

The eval grammar gains the namespace surface: `fset`, `defalias`,
`funcall`, `apply`, `function`, `#'`, designator chains, and
`fmakunbound`-under-live-callables.  Add `#'` and the new names to
`fuzz/fe.dict`.  Replay the tracked seeds.  This slice moves callables'
GC-visible homes; per the standing 03F lesson, the fuzz lane is the one
that vouches for rooting changes — run fe's full `.ci/run-ci-steps.sh`,
not just `make check`.

## Tests owned by this slice

- `test_api.c`: the cut's direct assertions — `(boundp 'car)` nil /
  `(fboundp 'car)` t; value-position use of a primitive name is
  `void-variable`; `(let ((car 5)) (car ...))`; `#'x` reads as
  `(function x)` (compare structure, not printing); `FeDefineNative`
  registers into the function cell (observable via `symbol-function`);
  context reuse after each new error.
- 04C's `TestFunctionCells` and `scripts/lisp2-basics.fe` continue to
  pass **unedited** — they were written against the final semantics, and
  surviving the fallback deletion untouched is their point.

## Gates

This is the end of the fe workstream before a pin: `make -C fe check` in
full, `complexity-check`, `pmccabe-check` (both inside 04A's caps;
record actuals against the funded row per Rule 6), `format-check`,
`compat`, and the **full nine-stage `fe/.ci/run-ci-steps.sh`** — valgrind,
ASan/UBSan, MSan, fuzz smoke, clang-analyzer, the works.  fe standalone
must be entirely green while kg's pin still points at 04C; that window is
normal and short-lived, and it is why 04E exists as a separate slice.

kg: **nothing**.  No pin move, no kg commit.  kg's tree still builds and
passes against the 04C pin.

## What this does not do

- **No kg pin, no kg source, no kg docs.**  All of it is 04E, atomically.
- **No compatibility residue.**  No value-cell fallback flag, no alias
  keeping `#'` identity reachable, no lax mode — §0.4 has no constituency
  to serve.
- **No strict arity, no conditions, no integers.**  The new diagnostics
  stay message-level; arity behaviour of the new forms was fixed in 04C.
- **No `let`-into-core and no prelude opinions.**  Fe's `let` primitive is
  untouched; what kg's prelude does about shadowing it is 04E's `defalias`
  sequencing, already decided in 04A.
