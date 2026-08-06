# 07A — Pin the arity target and fund the phase

Parent: [Phase 7](../2026-08-03-elisp-subset-and-fe-evaluator.md#11-phase-7--strict-arity-and-interactive-command-arguments).
Like 04A/05A/06A: measure the target before any implementation exists,
record the binding decisions, and price the work. This is a
tests/manifest/planning slice: both repositories gain planned compat cases
and fresh oracle snapshots; all three Fe complexity budgets and kg's total
budget move; the parent/set documentation records why. Neither repository
gains runtime behaviour.

## Outcome

Every Emacs-comparable behaviour Phase 7 will implement has a measured
Emacs 31.0.90 oracle row checked in before the implementation starts;
host-only behaviour has an explicit unit contract instead. The parent plan's
false ordering premise is corrected in writing, the binding decisions below
are recorded, and both repositories' complexity gates are funded for the
phase from current, green measurements.

## The audit's headline, and Decision 1 (ordering)

The parent plan (§11) and `doc/fe-upstream.md` both assert that strict
arity cannot be enabled before interactive argument construction lands,
"because kg invokes interactive commands with zero arguments". The
audit **measured** the opposite: with `FeSetStrictArity(true)` patched
into `kg_lisp_init`, kg passes 406/406 PTY cases and every native
suite, because every Lisp-defined command in the entire corpus is
`(defun name () (interactive) …)` — zero required parameters — and the
one exception uses `&optional`. The constraint is real only as
*forward* compatibility: the first user who writes
`(defun c (x) (interactive "p") …)` needs the arguments.

**Decision 1: Fe's strict-arity work lands first (07B), and the pin
move (07C) is an atomic kg commit that needs no kg runtime adaptation
and no existing expectation edit — proven by the suites staying green.
The pin still exposes strict calls and structured native-arity conditions.
Interactive arguments (07D/07E) then make strictness useful for
commands with required parameters.** 07A corrects the parent §11
sequencing paragraph now. `doc/fe-upstream.md` remains a description
of the shipped pin and changes in 07C; its blocker row then says
"strict arity was independently landable; interactive arguments make
required-argument commands usable".

## The measured oracle (Emacs 31.0.90, `/opt-3/emacs-31-lucid`)

Every Emacs row below was measured in `--batch` with `lexical-binding: t`
during the 2026-08-06 audit. Rows marked **[case]** get a compat case
+ fresh oracle snapshot in this slice, before implementation; the
rest are recorded here as the reference the implementation cites. A8 is
explicitly a Fe host-API unit case, so it has no fictitious Emacs oracle.

### A — lambda arity and the condition object

| id | expression | Emacs |
|---|---|---|
| A1 **[case]** | `(funcall (lambda (x) x))` | `(wrong-number-of-arguments #[(x) (x) nil] 0)` — data is `(FUNCTION NARGS)`, a two-element list; NARGS is the actual count, both directions |
| A2 **[case]** | `(funcall (lambda (x) x) 1 2)` | same condition, NARGS 2 |
| A3 | `(eval '(car 1 2) t)` vs `(funcall 'car 1 2)` | FUNCTION is the **symbol** from `eval`, the subr object from `funcall`, the raw list for an uncompiled lambda |
| A4 | `(error-message-string '(wrong-number-of-arguments car 2))` | `"Wrong number of arguments: car, 2"` |
| A5 | hierarchy | `(wrong-number-of-arguments error)` — depth 1, already in fe's static table |
| A6 **[case]** | too many arguments to a fixed primitive, with side effects in the operands | arity is rejected **before any operand is evaluated**: `(setq n 0)` stays 0 after `(car (setq n 1) (setq n 2))` is caught |
| A7 **[case]** | too many lambda arguments, with side effects in the operands | operands are evaluated left-to-right **before** lambda binding diagnoses arity: `n` ends as 2 after `((lambda (x) x) (setq n 1) (setq n 2))` is caught |
| A8 **[Fe unit]** | too many arguments to a host native | Fe evaluates operands before entering the native; its `FeRequireNoArguments` then raises. This phase fixes condition/data, not that ordering — Fe's native registration API declares no arity |

### L — lambda lists (diagnosed at call time, as `invalid-function`)

| id | lambda list | Emacs | fe today |
|---|---|---|---|
| L1 **[case]** | `(&rest)` | `invalid-function` | prose `error` |
| L2 | `(&optional)` | tolerated, body runs | tolerated ✓ |
| L3 **[case]** | `(&rest x y)` | **tolerated** — `x` gets the rest, `y` ignored | error — fe stricter |
| L4 | `(&optional a &optional b)` | `invalid-function` | tolerated |
| L5 **[case]** | `(1)` non-symbol param | `invalid-function` | strict: prose error |
| L6 | `(x x)` duplicate | tolerated, later wins | tolerated ✓ |
| L7 | `(a . b)` dotted, bare-symbol `(lambda x x)` | `invalid-function` / internal `cl-assertion-failed` leak | **legal Fe rest spellings** — recorded divergence, kept |
| L8 **[case]** | `((lambda (a &optional b) (list a b)) 1)` | `(1 nil)`; missing optionals bind nil; required still required (0 args → wna) |
| L9 **[case]** | `((lambda (a &rest r) (list a r)) 1)` | `(1 nil)` — empty rest is nil; the rest list is **freshly consed per call** (measured via setcar and via `apply` eq-ness; fe already matches) |

### V — variadic and primitive contracts

| id | expression | Emacs | fe today |
|---|---|---|---|
| V1 **[case]** | `(and)` | **`t`** | `nil` — wrong answer, not missing check |
| V2 **[case]** | `(car 1 2)`, `(cons 1 2 3)`, `(not 1 2)`, `(quote 1 2)` | `wrong-number-of-arguments`, NARGS actual, before operands run | silently ignored |
| V3 **[case]** | `(car)`, `(cons 1)` | `wrong-number-of-arguments` | prose "too few arguments", condition `error` |
| V4 **[case]** | `(if)`, `(if 1)` | `wrong-number-of-arguments` | `(if)` → nil |
| V5 **[case]** | `(print)`, `(print 1 nil)`, `(print 1 nil 2)` | wna / accepted / wna | Fe's `print` is a different, variadic stdout primitive; Decision 4 pins the narrower cut |
| V6 **[case]** | `(signal 'error)` | **accepted** — signal is `(1 . 2)` | fe requires exactly 2 |
| V7 | `(+)` `(-)` `(*)` `(/)` `(/ 4)` `(= 1)` `(< 1)` `(/= 1)` | `0 0 1 wna 0 t t wna` | fe matches all ✓ (condition symbols aside) |
| V8 | `(max)` `(min)` | wna | fe has neither primitive — out of scope |

The implementation census is wider than the examples above. 07A freezes the
following exhaustive inventory; 07B encodes it in a `Primitive`-indexed
table whose compile-time size check makes a newly added enum value visible:

- exact zero (`env`, Fe policy), exact one (`quote`, `function`, the unary
  family), exact two (Fe's one-binding `let`, the binary family, `set`,
  `throw`, `eq`/`eql`/`/=`), and one-or-two (`signal`);
- minimum arity (`if` 2+, `while`/`unwind-protect`/`catch`/`error` 1+,
  `condition-case` 2+, `funcall`/`apply` 1+);
- genuinely variadic (`and`, `or`, `do`, `list`, `setq`, the arithmetic
  and chained comparison families), including their existing zero/minimum
  identities; and
- closure constructors (`lambda`/`fn`/`macro`) with a raw parameter-list
  minimum, followed by lambda/macro *invocation* in `ArgsToEnv`, whose call
  arity is checked after ordinary operands/raw forms have been collected.

Fe-only names are labelled `fe-policy`, not presented as Emacs oracle
claims. `print` remains a recorded Fe divergence: it keeps its 1+
multi-value stdout contract, but the accidental zero-argument blank line
goes away. This avoids pretending that accepting Emacs' optional output
stream while printing that stream object as a second value is compatible.

### I — interactive (all measured; implemented in 07D/07E)

| id | behaviour | Emacs |
|---|---|---|
| I1 | `p` | `prefix-numeric-value`: nil→1, `(4)`→4, `(16)`→16, 5→5, `-`→−1 |
| I2 | `P` | raw prefix unchanged: nil, `(4)`, 5, `-` |
| I3 | `r` | `(min max)` sorted; mark==point legal; no mark → plain `error` "The mark is not set now, so there is no region" |
| I4 | `(interactive)` nil spec + required args | **no nil padding**: `call-interactively` raises `wrong-number-of-arguments`, identically to `funcall` |
| I5 | `(interactive FORM)` | evaluated at invocation, before the body; must return a list (`(interactive 5)` → `wrong-type-argument listp 5`); sees `current-prefix-arg` |
| I6 | unknown/deferred code | a truly unknown `Y` is an invalid-control-letter error, but a valid code outside this phase's subset must not be misclassified; Decision 6 records the complete measured set |
| I7 | `n` vs `N` | **`n` ignores the prefix arg and prompts; `N` uses the prefix without prompting when present** — easy to get backwards |
| I8 | `b` vs `B` | `b` default = current buffer, existing required; `B` default = other-buffer, new name allowed |
| I9 | `f` vs `F` | `f` existing file; `F` new file allowed |
| I10 **[case]** | declaration placement | only the first body form after an optional docstring is a declaration; a later `(interactive …)` does not make the function a command and remains an ordinary nil-valued body form |
| I11 **[case]** | empty/nil spec and redefinition | `(interactive)` and `(interactive nil)` both supply zero arguments; redefining an interactive function without the declaration makes it non-command immediately |
| I12 **[case]** | raw prefix capture | no prefix→nil, `C-u`→`(4)`, `C-u C-u`→`(16)`, digits→integer, bare `M--`→`-`, and `M-- 5`→−5 |
| I13 **[case]** | string grammar | one clause per newline; the first byte is the code and the rest is its prompt; `r` contributes two arguments and an empty string contributes none |
| I14 **[case]** | Lisp-to-Lisp command execution | `command-execute` invokes a Lisp-defined command through its interactive specification and propagates an arity/error completion to the surrounding Lisp evaluation |
| I15 **[case]** | lexical capture in FORM | a command closure created inside lexical `let` may reference that captured value from its interactive FORM; evaluating detached raw syntax in the global environment is wrong |

## Decisions

**Decision 2 — the condition, its data, and when it fires.** Both
too-few and too-many raise `wrong-number-of-arguments` (already in Fe's
hierarchy) with data `(FUNCTION NARGS)` per A1/A2. The call frame carries
the original designator and actual count instead of trying to reconstruct
either after operands have been consumed: direct symbol-head calls report
that symbol; computed/`funcall` calls report the callable object; lambdas
report the closure object. Fe's printed closure form differs from Emacs'
`#[…]`, so lambda-arity compat cases compare the condition symbol and
NARGS (extracted Lisp-side), not the rendered FUNCTION, and the rendering
difference gets a divergence row.

Core fixed/minimum arity is checked from the raw argument list in primitive
dispatch, before evaluation (A6), not from `ResumeUnary`/`ResumeBinary`
after a side effect has already happened. Lambda and macro arity stays in
`ArgsToEnv`, after operand/raw-form collection (A7). Native calls publish
their callable and original argc in a scoped context record so
`FeGetNextArgument`/`FeRequireNoArguments` can construct the same data;
nested native calls must save and restore that record. Their "too few
arguments" / "too many arguments" messages remain byte-identical while
the condition becomes `wrong-number-of-arguments` — the same
rendered-text-preserving reclassification 06D used. A8 is an explicit
divergence: without a declared-arity native registration API, helper-based
native checks remain post-evaluation; this phase does not invent one while
removing the unrelated strictness switch.

**Decision 3 — malformed lambda lists.** `(&rest)` and non-symbol
parameters become `invalid-function` per L1/L5 (the condition exists).
Fe's two deliberate rest spellings (L7) stay as recorded divergences.
Fe stays *stricter* than Emacs on L3/L4 (`&rest x y`, double
`&optional`) — rejecting garbage Emacs merely tolerates is not an
incompatibility any real init file can feel; each gets a divergence
row with the measured Emacs answer, and all rejected malformed-list
shapes use `invalid-function` rather than unrelated prose `error`.

**Decision 4 — primitive arity.** The inventory, not an ad-hoc switch
list, is the acceptance gate. Fixed/minimum contracts reject in dispatch
before evaluation; `(and)` answers `t` (V1); `signal` loosens to 1–2
arguments (V6); `print` becomes 1+ while retaining its documented Fe
stdout semantics. All arity raises carry Decision 2's identity and actual
count. `max`/`min` stay out of scope (V8) — no producer asked for them.

**Decision 5 — the API break and versions.** `FeSetStrictArity()` and
`FeGetStrictArity()` are removed outright (§0.4: no deprecated no-op,
no lax mode), with `main.c`'s `-a` flag and `test.sh`'s third pass —
the third pass becomes redundant because passes one and two *are*
strict, and its freed budget buys `scripts/arity.fe` + goldens (the
negative-arity script). `FE_API_VERSION` 5 → **6** (public functions
removed), `FE_LANGUAGE_VERSION` 5 → **6** (existing programs change
meaning — the two-axes 06D precedent), `FeVersion` `"6.0"` → `"7.0"`.
kg's static_asserts fire at the pin, as designed.

**Decision 6 — interactive scope and representation.** 07D implements
declaration placement/redefinition, the no-arg spec, `p`, `P`, `r`, and
`(interactive FORM)` (the escape hatch that makes deferred letters
non-blocking; metadata roots a zero-argument invocation thunk so the FORM
keeps its lexical environment). 07E implements
the prompting letters `n`/`N`, `s`, `f`/`F`, `b`/`B` on a new
Lisp→minibuffer seam. String specs use the newline grammar in I13.
The parent plan's "parsed interactive specification" means a rooted
descriptor kind plus value — nil/string directly, or a zero-argument thunk
for FORM — and invocation-time parsing/calling. Invalid string codes, like
FORM evaluation, fail when the command is invoked, not while `defun` loads.
Raw FORM syntax is not separately retained; that is why
`interactive-form` reflection remains out of scope.

The parser distinguishes an invalid letter from a valid-but-deferred Emacs
code. The measured valid set is
`a b B c C d D e f F G i k K m M n N p P r R s S U v x X z Z`, with
leading `*`, `@`, and `^` as modifiers rather than argument codes. This
phase supports only the subset named above; every other valid code reports
"unsupported interactive code CODE", while a letter outside the measured
set reports "invalid interactive code CODE". Deferred codes, modifiers,
prompt `%` interpolation, `interactive`'s MODES metadata, and
keyboard-macro strings as commands get explicit divergence rows.

`struct command_prefix` remains editor-owned and Fe-free: it stores the
effective integer plus a small raw-kind enum (none, integer, repeated
universal, minus). `P` and the temporary Lisp value of
`current-prefix-arg` are constructed at the Lisp boundary. Bare `M--` and
negative digits join the keyboard parser in 07D so every I12 form is
actually reachable. `current-prefix-arg` is a value binding during command
argument construction and the body, not an invented zero-argument function;
the top-level command runner saves/restores it on normal, error, quit, and
budget exits. `prefix-numeric-value` is a function.
Fe has no special variables: a lexical binding named `current-prefix-arg`
shadows this command-boundary global, unlike Emacs' dynamically bound special
variable. Record that divergence explicitly; Phase 7 does not add general
dynamic binding to Fe.

Lisp-defined commands gain `CMD_LISP_CALLABLE`, but not `CMD_REPEATS`: per
Emacs, a prefix reaches an interactive Lisp command as data, never as a kg
repetition count. `command-execute` uses the ambient command prefix when
there is one and invokes a Lisp command inside the active evaluator instead
of entering the top-level `state.frame_active` refusal path.

## Corrections this slice records

- Parent §11's ordering premise — false as a present-tense claim
  (Decision 1); 07A corrects it and adds the funding section below to
  the parent, the way 06A did.
- `doc/fe-upstream.md:160-165`'s blocker row — rewritten at the 07C
  pin, not before (it describes the shipped lax pin until 07C).
- kg's `after-change-functions` passes 4 arguments buffer-first where
  Emacs passes 3 (`BEG END PRE-LENGTH`). Strict arity makes the
  divergence load-bearing for ported code. Phase 7 records it as a
  first-class divergence row (07C) and defers any signature change to
  Phase 8's init-file compatibility work.
- fe's compat comparator checks the condition symbol and (since the
  Phase 6 review fixes) data lists — the arity rows are the first
  family to lean on the data comparison; 07B must confirm
  `records_agree` exercises it.

## Funding

Measured 2026-08-06 from an idle tree, at the Phase 6 close: Fe scc
**654/680**, `fe_eval.c` **440/460**, pmccabe **863/900 across 292
symbols**, worst function `DispatchPrimitive` 15/22; kg scc
**5489/5500** — 11 points of headroom. Re-take all four floors at
slice start; these are the reviewed floor, not literals a later tree
must reproduce.

Fe's half (07B) now includes call-site identity/argc state, a central raw
arity preflight, `ArgsToEnv` validation/data construction, native-helper
classification, and fuzz builders: **+50..80 scc / +50..80 pmccabe**.
That does not fit any current Fe headroom (26 total, 20 in `fe_eval.c`,
37 pmccabe). 07A therefore raises all three funded caps:
`SCC_COMPLEXITY_MAX` 680 → **760**, `SCC_FILE_COMPLEXITY_MAX` 460 →
**520**, and `PMCCABE_TOTAL_MAX` 900 → **980**, recording the measured
floor beside each Makefile Decision. `ArgsToEnv` is already pmccabe 13;
if it or a new helper exceeds the new-function limit 15, split the logic
rather than pricing an exception.

kg's half (07D+07E) — declaration metadata and redefinition, the prefix
representation/parser, bounded argument construction, nested command
execution, and the non-mutating prompt seam — is priced **+110..170
scc** (+80..120 in 07D, +30..50 in 07E). It does not fit in 11 points.
07A raises kg's `SCC_COMPLEXITY_MAX` 5500 → **5660**, 171 points above
the measured 5489 floor, with that floor recorded beside the Makefile
Decision. kg's per-file cap (520) and pmccabe gates are expected to hold;
record actuals per slice.

## Gates

The slice's own: the new `[case]`-marked compat cases exist with fresh
oracle snapshots (house workflow, never regenerating an existing
snapshot), Fe's `compat` and kg's `lisp-compat-check` report them as planned
gaps (07B/07D/07E turn them green), the reviewed primitive inventory covers
every enum value, all funded caps are proved live by temporary lowering,
both repositories' complexity and pmccabe gates are green at the new caps,
and the funding text is added to the parent plan and set README price table.

## What this does not do

- No implementation — every new case lands failing-by-design as a
  known gap, exactly like 05A/06A.
- No kg source, no pin move.
- No `func-arity`, no `interactive-form` reflection, no keyboard-macro
  commands, and no valid interactive code outside Decision 6's subset —
  recorded exclusions.
- No implementation of prompt modifiers/interpolation, deferred letters,
  `interactive` MODES, or general dynamic binding (Decision 6).
- No decision on Emacs' exact rendered arity *messages* — kg keeps its
  prose (Decision 2); `error-message-string` parity stays out of scope as
  recorded in Phase 6.
