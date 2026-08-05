# 05D — The numeric cut

Parent: [Phase 5](../2026-08-03-elisp-subset-and-fe-evaluator.md#9-phase-5--add-integers-and-complete-numeric-operations),
fe-only — **and deliberately without a kg pin move**, the 04D pattern:
the hard cut lands in the owning repository, passes its full standalone
CI, and kg adapts in the next slice's separate atomic commit.  A
pin-only commit cannot build a working kg (the prelude's `eq` alias
collides with the new primitive at `lisp-compat-check`, and every
kg-side numeric expectation shifts), which is the state Rule 10's
clause anticipates.  05E is the pin.

**Prerequisite:** [05C](05c-the-numeric-tower.md).  The tower must be
complete and fuzzed before the reader starts feeding it integers.

## Outcome

Fe has two numeric types end to end.  `42` reads as an integer and
prints as `42`; `42.0` reads as a float and prints as `42.0`; `0x10`,
`inf` and `nan` are symbols; `(/ 7 2)` is `3`; `eq` and `eql` exist
with Emacs semantics; the nonfinite floats print in Emacs' spellings
and read back; and the language version says so.  The long-pinned
`reader-no-integers` divergence flips to `supported` un-regenerated.

## Files this slice owns

`fe/fe.c` (the reader's number lexer, the printer, the `eq`/`eql`
primitives' names, bootstrap), `fe/fe_eval.c` (`PEq`/`PEql` arms),
`fe/fe_internal.h`, `fe/fe.h` (`FE_LANGUAGE_VERSION` 3 → 4;
`FE_API_VERSION` 3 → 4 iff 05A's placement Decision renumbered the
enum — the bump lands here either way so the whole phase is one
visible break; `FeVersion` `"4.0"` → `"5.0"`), `fe/test_header.c`,
`fe/example_host.c`, `fe/test_api.c`, `fe/scripts/*` and `fe/tests/*`
where output legitimately changed, `fe/compat/features.json` + the
flipped entries, `fe/fuzz/fe.dict` (numeric tokens — it has none
today) and the reader-fuzz corpus, `fe/README.md`, `fe/doc/*.md`.

## The cut, site by site

1. **The reader.**  `ReadAtom`'s `strtod` block (`fe.c:958-961`)
   becomes the 05A Decision-3 lexer: classify the token first
   (integer / float / nonfinite spelling / symbol), then convert with
   `strtoll`/`strtod` on the classified text only.  Overflowing
   integer literals per the Decision (recommended: fall back to
   double, divergence row).  The existing `.`-and-dotted-pair pins
   (`test_api.c:1996-2016`) must pass unedited.
2. **The printer.**  `EmitDouble`'s integral shortcut dies; doubles
   print via the shortest-round-trip loop with the `.0` guarantee and
   the Emacs nonfinite spellings (05A Decision 4).  Integers were
   already printing `%PRId64` since 05B.
3. **`eq` and `eql` land** (05A's collision table): pointer identity
   or integer-value equality for `eq`; `eql` adds same-type float
   equality by bits.  Two `FeFrameBinary` leaves.  fe's manifest
   claims both names — kg's `lisp-compat-check` will fail on the
   *next* pin unless 05E deletes the prelude alias in the same
   commit, which is exactly the designed tripwire.
4. **Bootstrap arithmetic constants.**  `pi` and `e` stay floats;
   nothing else in the bootstrap is numeric.  Audit rather than
   assume.
5. **Scripts and goldens** — 02C's method, read all 30, not grep:
   - `scripts/mandelbrot.fe` divides positions by `(/ height 2)` and
     `(/ width 3)`; under integer literals those truncate and all
     150 000 pixel lines of its golden change.  **Migrate the script**
     (`150.0`-style spellings where float math was meant) so the
     golden stays byte-identical — the golden is the proof the
     migration preserved the program, the 02C rule.
   - `scripts/math.fe`'s golden changes *legitimately* (float
     printing: `3.141593` → the round-trip spelling; `(/ 1 0)` → the
     script must choose: spell `(/ 1.0 0)` to keep printing `inf`'s
     new spelling, or keep integer division and expect the
     `arith-error` — pick per what the script is demonstrating and
     say so in the commit).
   - The frame-trace goldens print loop counters `1`/`2` — integers
     now, same text; byte-identical, and a moved trace is a finding.
   - Every other numeric golden line (`macros.fe`'s `0`–`9`,
     `lisp2-basics.fe`'s answers) is integer-valued and unchanged.
   - The `-a` pass stays byte-identical with the other two.
6. **Compat flips**: `reader-no-integers` → `supported` (rewrite its
   rationale — it currently *explains* the one-type model), the 05A
   reader/printer/equality rows → `supported`, the A8 overflow row
   and the int64-literal-overflow row stay `divergent` with their
   bignum rationales.  Audit every numeric entry's comparison still
   passes; `make -C fe compat` green with no snapshot regenerated.
7. **Fuzzing.**  `fuzz/fe.dict` gains integer/float/nonfinite tokens;
   `BuildNumber` already emits both types (05B); replay all tracked
   seeds; run the reader fuzzer long enough to trust the new lexer
   (the lexer is exactly where `symbol too long`/partial-token bugs
   live).

## Versions, and what each claims

- `FE_LANGUAGE_VERSION` **3 → 4**: literal syntax, printing, division
  results, unary seeds, comparison chaining and the equality family
  all changed meaning — the same axis as Phase 2's cut.
- `FE_API_VERSION` **3 → 4**: `FeMakeInteger`/`FeToInteger` joined in
  05B under the old number and the `FeType` enum may have renumbered
  (05A placement (a)); the bump makes the numeric contract one
  visible break, and kg's `static_assert(FE_API_VERSION == 3)` firing
  at the next pin is the designed tripwire.  If 05A chose placement
  (b) *and* nothing else broke binary compatibility, record the
  Decision's reasoning and bump anyway — two axes moving together is
  this program's precedent (04D), and a "compatible" break that
  silently reinterprets `42` is the worst kind.
- `FeVersion` **"4.0" → "5.0"**.

## Tests owned by this slice

`test_api.c`: the cut's direct assertions — every 05A R/P row as a
read-or-print test (`1.` → integer, `.5` → float, `0x10` → symbol,
`9007199254740993` exact, `-0.0` round-trips, `42.0` prints `42.0`,
nonfinite spellings both directions); `eq`/`eql` per the E rows;
`(/ 7 2)` → `3` through the reader at last; context reuse after each
new error.  The 05C `TestNumericTower` suite passes **unedited** —
written against final semantics through the host API, surviving the
cut untouched is its point.

## Gates

The end of the fe workstream before a pin: `make -C fe check` in full,
`complexity-check`, `pmccabe-check` (inside 05A's caps; record actuals
per Rule 6), `format-check`, `compat`, and the **full nine-stage
`fe/.ci/run-ci-steps.sh`** (the known `ulimit -s unlimited` note for
`TestRootsAndCalls` stands).  fe standalone entirely green while kg's
pin still points at 05C; that window is normal and short-lived.

kg: **nothing**.  No pin move, no kg commit.

## What this does not do

- **No kg pin, no kg source, no kg docs.**  All of it is 05E.
- **No compatibility residue** — no double-reader flag, no old-printer
  mode, no `is`-aliased `eq` reachable.  §0.4.
- **No bignums, no radix syntax, no character literals.**  Recorded
  divergences or Phase 8 questions, per 05A.
- **No prelude opinions** — what kg does about `eq`'s alias, `equal`'s
  tail and `zerop` is 05E's, already decided in 05A's table.
