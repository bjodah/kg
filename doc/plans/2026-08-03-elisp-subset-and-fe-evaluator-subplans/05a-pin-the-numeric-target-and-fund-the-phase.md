# 05A — Pin the numeric target and fund the phase

Parent: [Phase 5](../2026-08-03-elisp-subset-and-fe-evaluator.md#9-phase-5--add-integers-and-complete-numeric-operations).
No behaviour changes in this slice.  Its outputs are oracle snapshots,
five recorded Decisions, one funded complexity raise, and corrections to
the parent's stale claims — the 00A/02A/04A pattern.  Everything later in
the set implements what this slice pins; nothing later re-litigates it.

## Why this slice exists

Phase 5 has more independently decidable questions than any phase so far
— reader grammar, printer representation, five equality operators, per-
function math-native return types, overflow policy — and every one of
them is answerable *today* by asking the pinned Emacs.  It is also, like
Phase 4, unfundable as it stands: fe's gates sit at **459/480 scc and
660/690 pmccabe** (21 and 30 points of headroom, measured 2026-08-05
after the Phase 4 review fixes) against a row priced **+50 to +70**.  A
funding Decision is mandatory before 05B writes a line.

## Outcome

- A `lisp2`-style family of new oracle cases in `fe/compat/` (planned
  entries, snapshots generated from the pinned Emacs 31.0.90 — new
  snapshots only; regenerate nothing), covering the answer table below.
- Five dated Decisions in the set README: representation, the equality
  family, reader grammar, printer representation, and error policy.
- The funded cap raise, proved live by 03A's temporary-lowering trick.
- The parent-plan corrections recorded (below).

## Parent-plan corrections to record

Verified against the audited tree, 2026-08-05:

- **"ten call sites" undersells the funnel.**  There are exactly 10 raw
  `FeMakeDouble`/`FeToDouble` lines in `src/lisp_*.c`, but 13 of the
  position-returning natives share one constructor,
  `lisp_position()` (`src/lisp_buffer.c:181`), and every numeric
  *argument* funnels through `lisp_finite()` (`src/lisp_buffer.c:115`).
  The kg cutover is a handful of choke-point edits plus two `format`
  type gates (`src/lisp_io.c:82`, `:115`) — smaller than the parent
  feared, and 05E's checklist is written from the funnel, not the grep.
- **`ARITH_OP`/`NUM_CMP_OP` no longer exist** — deleted with the
  recursive evaluator (03E).  The tower extends `ResumeArith`/
  `ResumeBinary`/the `PNumericEqual` arm, not macros.
- **§0.2's "split fe.c" is long settled** (03B).
- **`=` is already chained-numeric with `wrong-number-of-arguments` and
  `wrong-type-argument` pinned** (Phase 2, plus the 2026-08-05 review's
  zero-operand identities: `(+)`→0, `(*)`→1, `(-)`→0, `(/)`→error).
  The parent's `(= 3 2)`/`(setq x 3)` acceptance cases pass today.

## The oracle answer table

One `compat/cases/num-*.json` + snapshot per row, all `planned`, each
rationale naming Phase 5 and the implementing slice.  Answers below are
predictions to be *confirmed by the oracle*, not copied into it; where a
prediction fails, the snapshot wins and the Decision records the
surprise.  (Emacs prints integers bare and floats with a decimal point
or exponent; `most-positive-fixnum` on the pinned build is 2^61−1, so
int64 edge cases near 2^62 exercise Emacs *bignums* — expect the
divergence rows to record that.)

| # | Case | Prediction | Pins |
|---|---|---|---|
| R1 | `(type-of 42)` / `(type-of 42.0)` | `integer` / `float` | the split exists |
| R2 | `1.` | integer `1` | trailing-dot integers |
| R3 | `.5` | float `0.5` | leading-dot floats |
| R4 | `+5` | integer `5` | sign prefixes |
| R5 | `1e3` | float `1000.0` | bare exponent is float |
| R6 | `(setq 0x10 7)` then `0x10` | `7` — `0x10` is a **symbol** | strtod's hex must die |
| R7 | `(boundp 'inf)` / `(boundp 'nan)` | `nil` — symbols, not numbers | strtod's inf/nan must die |
| R8 | `(read "1e")` — spelled as a symbol probe like R6 | symbol | partial exponents |
| R9 | `9007199254740993` | prints `9007199254740993` | int64 exactness past 2^53 |
| R10 | `-0.0` | prints `-0.0` | signed zero survives read→print |
| P1 | `42.0` | prints `42.0` | the headline printer change (flips `reader-no-integers`) |
| P2 | `0.1` | prints `0.1` | shortest round-trip, not `%.7g` noise |
| P3 | `(/ 1.0 0)` | `1.0e+INF` | Emacs nonfinite spelling |
| P4 | `(- 0 (/ 1.0 0))` | `-1.0e+INF` | |
| P5 | `(sqrt -1)` | `-0.0e+NaN`-family text | NaN spelling (record exactly what Emacs prints) |
| A1 | `(- 5)` | `-5` | unary minus is negation (fe today: `5`) |
| A2 | `(/ 5)` | `0` | unary `/` is reciprocal, truncated |
| A3 | `(/ 7 2)` / `(/ -7 2)` | `3` / `-3` | truncation toward zero |
| A4 | `(/ 7 2.0)` | `3.5` | mixed promotes |
| A5 | `(/ 1 0)` | `arith-error` | integer division by zero |
| A6 | `(* 2 3)` | integer `6` | all-int stays int |
| A7 | `(+ 1 2.0)` | float `3.0` | promotion |
| A8 | `(* 4611686018427387904 4)` | Emacs: bignum; fe target: overflow error | the divergence row |
| C1 | `(< 1 2 3)` / `(< 1 3 2)` | `t` / `nil` | chaining (fe today ignores the third operand) |
| C2 | `(> 3 2 1)`, `(>= 2 2 1)` | `t` | the new comparators chain too |
| C3 | `(/= 1 2)` and `(/= 1 2 3)` | `t` and `wrong-number-of-arguments` | `/=` is strictly binary in Emacs |
| C4 | `(= 3 3.0)` | `t` | mixed numeric `=` |
| C5 | `(< 1 'a)` | `wrong-type-argument` | comparison error name (fe today: `expected double`) |
| E1 | `(eq 3 3)` | `t` | fixnums are `eq` by value — load-bearing for kg (`memq`, `auto-fill.el`) |
| E2 | `(eq 3.0 3.0)` | `nil` | floats are boxed |
| E3 | `(eq "a" "a")` | `nil` | strings are identity under `eq` — today's headline divergence dies |
| E4 | `(eql 3 3.0)` / `(eql 1.5 1.5)` | `nil` / `t` | `eql` is type-strict value equality |
| E5 | `(eql 0.0 -0.0)` | `nil` | bit-level float `eql` |
| E6 | `(equal 3 3.0)` / `(equal "ab" "ab")` | `nil` / `t` | `equal` = `eql` on numbers, content on strings |
| M1 | `(sqrt 16)` | `4.0` | float, always |
| M2 | `(expt 2 8)` / `(expt 2 -1)` / `(expt 2.0 8)` | `256` / `0.5` / `256.0` | expt's per-signature rule |
| M3 | `(floor 7.5)` / `(truncate -7.5)` / `(ceiling -7 2)` | `7` / `-7` / `-3` | the rounding family returns integers |
| M4 | `(round 2.5)` / `(round 3.5)` | `2` / `4` | round-half-even |
| M5 | `(sin 0)` | `0.0` | transcendentals are floats |
| Z1 | `(zerop 0)` / `(zerop 0.0)` / `(zerop "a")` | `t` / `t` / `wrong-type-argument` | |
| T1 | `(integerp 3)` `(integerp 3.0)` `(floatp 3.0)` `(numberp 3)` | `t` `nil` `t` `t` | the predicate family |

Some of these already exist (`primitive-div`, the `=` family, the
zero-operand pair) — audit `compat/features.json` first and add only
what is missing; the checker forbids duplicate coverage claims.

## Decision 1 — representation

`FeTInteger`, payload `int64_t` in the `Value` union.  Measured facts:
the union is 8 bytes with `FeDouble n` already its widest member, so
**`int64_t i` fits with zero object growth** (`fe_internal.h:79-85`);
the tag byte has headroom (`FeTSentinel` is 15 of ~31).

The open question is *enum placement*, because `fe.h`'s `FeType` is
public and `fe.c:130-134` asserts `FeTFex0 > FeTPtr`:

- **(a) insert `FeTInteger` immediately after `FeTDouble`** — the
  natural reading order, renumbers every later constant including the
  public `FeTPtr`, and is therefore an ABI-visible break that rides
  Phase 5's version bump anyway (05D).  Recommended: the enum should
  read sanely for the next decade, and the phase already breaks the
  language version; take the API bump with it.
- **(b) append before `FeTSentinel`** — no renumbering, no API bump,
  ugly forever.

Whichever wins: a `type_names[]` slot (`"integer"` — which kg's
`type-of` then returns with **zero kg code change**, flipping the
`native-type-of` divergence), a `WriteObject` arm, a `FexGC` arm
(`fex.c:15-36` has no `default:`), and Fex's own type-name registration
offsets checked.  GC needs nothing: an integer is a leaf exactly like a
double.  No new interned symbols, so `FeMinimumArenaSize()` does not
move in 05B (it moves in 05C with the new primitive *names*; predict
≈96 bytes per name, `1 + 5 + (len-1)/7` objects each, and record the
measured figure).

## Decision 2 — the equality family, and who owns each name

The constraint that shapes everything: `utils/check_lisp_compat.py`
requires every fe primitive name, kg native name and kg prelude
definition to be claimed exactly once *across both manifests*, and the
disjointness of the three name sets is checked at every kg pin.  So a
new fe primitive whose name kg currently defines **cannot land in a
slice whose pin precedes the kg-side deletion**.  The table:

| Name | Today | Target | Slice |
|---|---|---|---|
| `is` | fe primitive; doubles ≈-equal (`IsNearlyEqual`!), strings by content, else identity | stays, as fe's own broad comparator; extended to integers per the Decision (recommend: mathematical value across int/float, preserving today's `(is 1 1.0)` → `t`); kg docs present it as fe-native, not an Emacs form | 05C |
| `eq` | **kg prelude alias of `is`** (`lisp/prelude.el:67`) | fe core primitive: pointer identity *or* both-integers-equal (Emacs fixnum semantics — E1 is load-bearing for kg's `memq`, `auto-fill.el:39`, and PTY `111-lisp-string-natives`) | **05D** (collision: prelude alias deleted in 05E's pin commit) |
| `eql` | does not exist | fe core primitive beside `eq`: `eq`, or same-type numbers equal by bits for floats (E5) / value for integers | 05D (no collision, but it is `eq`'s sibling — keep them together) |
| `equal` | kg prelude lambda whose atom tail is `(is a b)` | prelude rewrite: spine loop unchanged; atom tail becomes strings → `string=`, numbers → `eql`, else `eq` | 05E |
| `string=` | kg native (`src/lisp_string.c:168`), byte equality | unchanged | — |
| `=` `<` `<=` | fe primitives (double-only) | extended in place | 05C |
| `>` `>=` `/=` | do not exist anywhere (`fe_internal.h:54` carries the TODO) | fe core primitives | 05C |
| `numberp` | **kg native** (`src/lisp_cmd.c:60`) | stays kg-native, widened to two tags — fe must **not** add a `numberp` primitive (collision, and `FeDefineNative` would silently overwrite it) | 05E |
| `integerp` `floatp` | do not exist | fe core primitives (no collision) | 05C |
| `zerop` | does not exist | kg prelude one-liner over `=` (no fe spend; `(zerop "a")` gets `wrong-type-argument` from `=` for free) | 05E |
| `1+` `1-` | kg prelude lambdas | untouched — integer-preserving automatically once `+` is | — |

`Equal()`'s epsilon comparison for doubles (`fe.c:365-383`) is *not* a
template for any of the new operators — `eq`/`eql`/`=` are all exact.
Record explicitly that `is` keeps its epsilon behaviour or loses it;
recommend keeping (fe's own scripts are the constituency), with the
sentence in `fe/doc/language.md` saying so.

## Decision 3 — reader grammar

Replace `ReadAtom`'s bare `strtod` (`fe.c:958-961`) with Emacs number
lexing: integer = optional sign, digits, optional trailing dot (R2);
float = digits with a fractional part and/or exponent (R3/R5); the
Emacs nonfinite spellings `1.0e+INF`/`0.0e+NaN` read as nonfinite
floats iff the printer emits them (Decision 4 — keep read/print
round-tripping as an invariant); **everything else is a symbol**,
which un-numbers `0x10`, `inf`, `nan`, `1e` (R6-R8).  Integer literals
that overflow int64: recommend reading as a double (the pre-bignum
Emacs behaviour) with a recorded divergence row against modern Emacs'
bignum; the alternative — a read error — punishes pasted constants.
Pin whichever with its own case.

## Decision 4 — printer representation

Integers: `%PRId64`.  Floats: Emacs prints the shortest representation
that reads back to the same value, always with a `.` or exponent.
Recommend the standard loop — `%.{1..17}g` until `strtod` round-trips
— in `EmitDouble`'s successor; it is deterministic, locale-independent
digits, and ~10 lines.  Nonfinite: adopt Emacs' `1.0e+INF` /
`-1.0e+INF` / NaN spellings (P3-P5), which retires the kg.1 §format
divergence whose rationale ("fe has only one number type") this phase
deletes.  The old integral-double shortcut (`fe.c:594-604`) dies with
the cut — after 05D a bare `42` *is* an integer, and `42.0` prints as
`42.0` (P1).

## Decision 5 — error policy

All message-level until Phase 6, matching the standing rule:
`arith-error` for integer division by zero (A5) and for int64 overflow
(A8 — a **recorded kg-policy divergence**: Emacs promotes to bignum;
fe refuses; use `__builtin_*_overflow`, never UB).  Comparison and
arithmetic type errors say `wrong-type-argument` (C5), replacing the
`expected double, got X` texts — `CheckNumericEqualOperand`'s comment
(`fe_eval.c:390-392`) explains why the oracle comparator needs the
Emacs name; the whole numeric family now follows it.

## The funding Decision

Measured 2026-08-05 (post-review): fe scc 459/480 (fe_eval.c 276/300),
pmccabe 660/690 across 248 symbols.  The row prices +50 to +70; the
review fixes spent 18 scc of Phase 4's funding on defects, so fund from
the measured floor: raise `SCC_COMPLEXITY_MAX` 480 → **540**,
`SCC_FILE_COMPLEXITY_MAX` 300 → **340**, `PMCCABE_TOTAL_MAX` 690 →
**760**, each proved live by temporary lowering, funding 05B–05D by
name.  kg: 5444/5500 (56 points) against a +15 to +25 row — no raise
expected; re-measure at 05E and record.  Record actuals per slice
(Rule 6) and bank any surplus at close.

## Data spikes (throwaway, do not land)

- Confirm `sizeof(Value)==8` holds with `int64_t i` added, on both CI
  compilers.
- Confirm the step-pin exposure: `TestEvaluationControl` pins
  `"(+ 1 2)"` at **exactly 4 steps** (`test_api.c:672-688`); the tower
  must not change arithmetic's step accounting, and 05C carries the
  pin forward unedited.  List every other exact-step pin
  (`test_api.c:3074`, `:3088`, `:3236`, `:3448`) and which touch
  numeric forms.
- Measure `GetCoreObjectCount()`/`FeMinimumArenaSize()` deltas for the
  planned primitive names (`>`, `>=`, `/=`, `integerp`, `floatp`,
  05D's `eq`/`eql`).

## Gates

fe: `make -C fe compat-oracle` (new snapshots only), `make -C fe compat`
(planned entries listed, not failing), `make -C fe check` untouched.
kg: pin move in its own green commit (the manifest gains nothing yet —
new fe cases claim no kg-owned names).  Complexity: the raise itself,
proved live.

## Landed — 2026-08-05

All five Decisions and the funding raise are recorded in the set README's
dated Decision section; this document's predictions are the record the
README summarises.  What actually happened, slice-close:

- **The corpus held, prediction for prediction.**  42 `num-*.json` cases
  and their version-stamped Emacs 31.0.90 snapshots landed in
  `fe/compat/` as `planned` entries (the answer table's 41 rows; C3 and
  Z1 each split into a value case and an error case, and P1 is covered by
  the existing `reader-no-integers` case, not duplicated).  `make -C fe
  compat-oracle` wrote exactly those 42 snapshots and **regenerated
  nothing** (83 unchanged, 0 failed); `make -C fe compat` is green at
  **132 case(s), 76 passed, 56 known gap(s), 0 failed**.  The ones worth
  doubting were confirmed: `(/= 1 2 3)` → `wrong-number-of-arguments`,
  `(eql 0.0 -0.0)` → `nil`, `(sqrt -1)` → `-0.0e+NaN`, A8 → bignum
  `18446744073709551616`.  The kg-side `lisp-compat-check` also demanded
  each planned rationale name a phase, which the rationales now do.
- **The funding landed as priced.**  `fe/Makefile`: `SCC_COMPLEXITY_MAX`
  480 → 540, `SCC_FILE_COMPLEXITY_MAX` 300 → 340, `PMCCABE_TOTAL_MAX`
  690 → 760, each proved live with the temporary-lowering trick
  (`SCC_COMPLEXITY_MAX=458` fails on 459, `SCC_FILE_COMPLEXITY_MAX=275`
  on `fe_eval.c`'s 276, `PMCCABE_TOTAL_MAX=659` on 660, and
  `pmccabe-baseline` refuses to launder the over-budget tree).  kg
  re-measured at **5450/5500** — 50 points, no raise, and this slice adds
  no `src/*.c` to either tree.
- **The three spikes measured, then deleted (only /tmp copies remain).**
  `sizeof(Value)` stays **8** with `int64_t i` on both gcc and clang;
  the exact-step arithmetic pins are five, led by `"(+ 1 2)"` at exactly
  4 steps (`test_api.c:672-688`), all numeric-form-carrying, all to
  survive 05C/05D unedited; and the seven planned primitive names cost
  **+43 objects / +688 B** (`GetCoreObjectCount()` 367 → 410,
  `FeMinimumArenaSize()` 55616 → 56304 B, kg 1 MiB 1098/56221 →
  1097/56225), the 04A finding in the same direction: the extra core
  objects ride inside the grown minimum, so the open-slot delta is +4,
  not −43, and one frame is lost to rounding.
- **No implementation, no status flips, no regeneration of existing
  snapshots, no kg manifest or prelude edits.**  fe's only diffs are
  `compat/` (cases, oracle, `features.json`) and the `Makefile` caps;
  kg's only source diff is the set README plus this document.  The kg pin
  moves to fe `59db1b9` in its own green commit, per Rule 10.

## What this does not do

- **No implementation.**  Not one reader, printer, or arithmetic line.
- **No bignums, ever, in this program** — int64 + recorded divergence.
- **No `number-to-string`/`string-to-number`, no `min`/`max`/`abs`/
  `mod`, no `?a` literals, no `#x` radix syntax** — none are in the
  parent's Phase 5 list; the first packages that need them are Phase 8's
  problem.
- **No snapshot regeneration** — new cases only.
