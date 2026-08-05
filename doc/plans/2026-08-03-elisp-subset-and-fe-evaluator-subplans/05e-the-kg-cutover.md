# 05E — The kg cutover

Parent: [Phase 5](../2026-08-03-elisp-subset-and-fe-evaluator.md#9-phase-5--add-integers-and-complete-numeric-operations),
kg-only: the pin move plus every kg adaptation in **one atomic green
commit**, per Rule 10 — the 04E shape, and it closes both the phase
and milestone 1.

**Prerequisite:** [05D](05d-the-numeric-cut.md), fully green in the
submodule including its nine-stage runner.

## Outcome

Every position, line, column, char code and count kg hands to Lisp is
an integer; `format` accepts both numeric types; the equality family
is Emacs' (`eq` strict, `eql`/`zerop` present, `equal` type-honest);
`(type-of 1)` says `integer` with zero kg code; the enumerated
expectation changes — and only those — land; and every "every number
is a double" sentence in the documentation is gone.

## Files this slice owns

The `fe` gitlink; `lisp/prelude.el` +
`src/lisp_prelude_generated.inc`; `src/lisp_buffer.c`,
`src/lisp_io.c`, `src/lisp_cmd.c`, `src/lisp_hooks.c`,
`src/lisp_string.c`, `src/lisp_core.c` (version asserts);
`test/test_lisp.c`, the enumerated `test/pty/*.yaml`,
`test/lisp-compat/` manifest + cases/snapshots,
`utils/check_lisp_compat.py` only if the claim-move needs it;
`doc/lisp-api.md`, `doc/fe-upstream.md`, root `README.md`,
`doc/kg.1`; the set `README.md` (phase- and milestone-closing Status).

## C-side checklist

The funnel makes this short; work from it, not from a grep:

1. **`lisp_position()`** (`src/lisp_buffer.c:181-184`) →
   `FeMakeInteger`.  One edit converts `point`, `point-min`,
   `point-max`, `mark`, `region-beginning`, `region-end`, all four
   search functions, `match-beginning`, `match-end`,
   `marker-position`, `bounds-of-thing-at-point`, and
   `after-change-functions`' start/end.
2. **The six remaining `FeMakeDouble` sites** → `FeMakeInteger`:
   `line-number-at-pos` (`lisp_buffer.c:264`), `current-column`
   (`:281`), `char-after` (`:501`), `string-length`
   (`lisp_string.c:63`), `string-to-char` (`:259`), the hook
   dispatch's `old_len` (`lisp_hooks.c:179`).
3. **Argument reads need nothing**: `lisp_finite()` uses `FeToDouble`,
   which 05B widened to accept integers — verify each derived reader
   (`lisp_offset_argument`, `lisp_optional_count`,
   `lisp_string_index`, `goto-line`, `char-to-string`,
   `match-beginning`'s index) against an integer *and* a float
   argument, and keep the existing clamp/truncate policies: they are
   kg's documented Emacs-matching behaviour, not doubles residue.
4. **`format`'s two type gates widen** (`src/lisp_io.c:82`, `:115`):
   `%d` and `%e`/`%f`/`%g` accept both numeric types (Emacs does —
   `(format "%e" 42)` works there), converting as needed; `%d` of a
   float keeps truncating (the `130-lisp-format-insert` case pins
   `87.6` → `87`); `%d` of a nonfinite float keeps refusing.  `%s`
   needs nothing — it is fe's writer.
5. **`numberp` becomes two-tag** (`src/lisp_cmd.c:60-63`) and its
   "every number is a double" comment dies.  `type-of` needs
   **nothing** — it reads fe's `type_names[]`, which is the whole
   reason the `native-type-of` divergence flips to `supported` here.
6. **Version asserts** (`src/lisp_core.c`): `FE_API_VERSION == 4`,
   `FE_LANGUAGE_VERSION == 4` per 05D.

## The prelude, per 05A's Decision 2

1. **Delete `(defalias 'eq (symbol-function 'is))`** — fe's `eq`
   primitive takes over; the manifest claim for `eq` moves from kg's
   prelude to fe's manifest in this same commit (the checker's
   disjointness rule is the tripwire that makes forgetting this a
   build failure, not a silent shadow).  Count drops 52 → 51; both
   spelling parsers (`utils/check_lisp_compat.py`,
   `test_lisp.c:test_prelude_source_file`'s `PRELUDE_DEFS`) move in
   step.
2. **`equal`'s atom tail** (`lisp/prelude.el:121`) stops leaning on
   `is`: strings → `string=`, numbers → `eql`, everything else →
   `eq`.  The spine loop is untouched (its shape is a deliberate
   frame-limit response — parent §Equality).
3. **`zerop`** joins as a one-liner over `=`.  `memq` keeps `eq` and
   keeps working on integers (fixnum `eq` — the E1 case is the
   guarantee); `member`/`assoc` keep `equal`.
4. **`lisp/auto-fill.el` needs nothing**: `(eq (char-after pos) 32)`
   is integer-`eq`, `(1+ pos)` is integer-preserving, `fill-column`
   comparisons are `<`/`<=`.  Its flipped-`<` workaround for the
   missing `>` may now be modernized or left; if modernized, its PTY
   cases must pass unedited — they are behaviour, not spelling.

Then `make lisp-prelude-generate` + `lisp-prelude-check`.

## Expectation changes — enumerated, or refused

Unlike Phase 4, this phase *deliberately* changes printed text, so
"no expectation edits" becomes "**only these** expectation edits, each
justified by a pinned oracle answer".  The allowlist, from the
2026-08-05 audit:

- **PTY** — `lisp-math-functions.yaml`: six of ten lines (`sin`→`0.0`,
  `cos`→`1.0`, `sqrt`→`4.0`, `atan`→full precision, `log`→`1.0`;
  `expt` stays `256` iff 05C implemented M2's integer rule — if that
  line needs editing, 05C got expt wrong).  `lisp-exponentiation.yaml`
  stays at `256` on the same argument.  **Every other Lisp PTY case
  passes unedited** — positions and counts print the same digits as
  integers, `eq`-on-integers holds (`111-lisp-string-natives`,
  auto-fill's embedded copies), `%d`-of-`(point)` holds
  (`131-lisp-message-format`, `lisp-process-filter-sentinel-order`).
  An edit to any case not named here is a red flag to investigate.
- **`test/test_lisp.c`** — the audited breakage table: the
  `(/ 1 0)`-family assertions (six sites near `:1834-1887`) respell as
  `(/ 1.0 0)` where the test is about nonfinite formatting, or move to
  the new `arith-error` expectation where it is about division;
  `(format "%d" 9007199254740993)` now prints exactly (drop the old
  rounding expectation); `(type-of 1)` → `"integer"`;
  `(round 2.5)`-family already Emacs-correct, now integer-typed with
  the same text.  Everything else in the 165-assertion numeric
  inventory is integer-valued text that must not move.
- **kg compat manifest** — `native-type-of` → `supported`;
  `format-exceptional-float` rewritten (its rationale describes the
  one-type model; `(/ 1.0 0)` under `%d` still refuses, now for the
  float reason alone); `prelude-eq`'s entry follows the claim move;
  new entries claim `eql`/`zerop` (prelude) and the widened behaviours,
  with new oracle snapshots generated in the house workflow —
  regenerate nothing existing.
- **`test/test_perf.c`** — nothing: all four evaluator-shape results
  are integers below 2^53 with identical text; a perf assertion that
  moves is a finding.

## Documentation rewritten in the same commit

- The "every number is a double" family: `README.md:422-426`,
  `:481-482`; `doc/lisp-api.md:516-517`; `doc/kg.1:597-601` and the
  `%s`/`%S` nonfinite-divergence block (`kg.1:432-453`) whose entire
  rationale rested on the one-type model — kg now prints Emacs'
  `1.0e+INF`/NaN spellings, so the divergence itself is retired.
- The `>`/`>=` absence notes (`README.md:471-475`,
  `doc/lisp-api.md:512-513`, `lisp/auto-fill.el`'s header comment if
  the workaround is modernized) — they exist now.
- The `eq`-by-value divergence (`README.md:476-477`,
  `doc/lisp-api.md:514-515`) — dead; replaced by the honest residue
  (`is` remains fe-native and broad; int64 not bignum; overflow and
  `arith-error` are message-level until Phase 6).
- `doc/lisp-api.md`'s Predicates/Lists rows gain
  `eql`/`zerop`/`integerp`/`floatp`/`>`/`>=`/`/=`; the format row's
  spec table reflects the widened gates.
- `doc/fe-upstream.md`: a new divergence-table row for the numeric
  cut (two types, int64-not-bignum, the version tuple
  `FeVersion "5.0"` / API 4 / LANGUAGE 4), the minimum-arena figure
  updated to 05C's measured value.

## Gates

Phase- and milestone-closing, Rule 9 in full: `make check` from an
idle tree, `make WITH_LISP=0 clean all check`, both complexity gates
(kg's row: +15 to +25 against 56 measured points — record actuals),
`header-check`, `docs-check`, `lisp-compat-check`,
`lisp-prelude-check`, `make lisp-compat-oracle` in its
verify-by-running form, and `JOBS=8 .ci/run-ci-steps.sh --parallel`
with a regenerated `compile_commands.json` before believing `ci-06`.

Then close the phase **and the milestone**: the Status entry records
per-slice actuals against 05A's funded row, what the plan got wrong,
and what is carried forward — and, because Phase 5 ends milestone 1,
it triggers the price table's own promise: **re-price the provisional
milestone-2 rows (Phases 6–9) against the real post-milestone-1
measurements** before any Phase 6 sub-plan is written.  Remove the
five sub-plan documents only after the reviewer accepts the completed
workstream.

## What this does not do

- **No `number-to-string`/`string-to-number`, no `min`/`max`/`abs`/
  `mod`, no `?a`, no radix syntax, no bignums** — 05A's standing
  exclusions.
- **No new kg natives**: the numeric surface is fe's; kg only widens
  what it already owns.
- **No clamp-policy changes**: `goto-char`'s clamping,
  `lisp_optional_count`'s saturation and `goto-line`'s bounds are
  documented behaviour and keep their shapes with integer inputs.
- **No condition objects** — `arith-error` is a message until
  Phase 6.
- **No expectation edit outside the allowlist above.**
