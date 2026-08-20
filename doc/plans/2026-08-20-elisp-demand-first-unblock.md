# Phase 29 -- demand-first: unblock the tree

Decided by the owner on 2026-08-20, from Phase 28's remeasurement
(`doc/plans/2026-08-20-elisp-phase28-remeasurement.md`, `5cd0e97`), over
the five structural branches: the 110-package census found ZERO
data-model first blockers, so the work goes where the blockers are.
The five branches stay evaluated and dormant exactly as Phase 27 does:
a named consumer reopens one, and the census is how a consumer gets
named.

## The measured backlog, in demand order

Each item carries its consumers as the census counted them.  Cost
classes use prior phases' actual sizes.

| # | Item | Named demand | Seam | Cost |
| --- | --- | --- | --- | --- |
| 1 | `defalias/3` (docstring arg), `require/3`, `regexp-opt/2` (PAREN) | 7 packages via dash.el:603; require/3 callers; regexp-opt PAREN is already a recorded oracle divergence | prelude / lisp_* natives | S |
| 2 | Reader: `"\(fn ...)"` docstring escape | cond-let.el:319 and the convention across ELPA docstrings; Emacs reads the character itself | fe reader (pin move) | S |
| 3 | `subr-x` as a providable module | 4 packages stop at `(require 'subr-x)` | prelude `provide` + whatever names those 4 reach next | S-M |
| 4 | `define-error`, and `signal` consulting `error-conditions` plists | s.el's own error path (measured wrong vs Emacs: `(error ("Invalid error symbol" sym))` where Emacs gives `(sym nil)`) | fe conditions (pin move) | M |
| 5 | `$`/`^` as CONTEXTUAL anchors (literal mid-pattern, as Emacs) | s-lex-format silently expands to an empty binding list today | tiny-regex-c -> fe -> kg pin chain | M |
| 6 | A cl-lib subset | 35 packages stop at `(require 'cl-lib)`; 12 more at `compat` | prelude module(s); U.0 must first measure what those packages reach for NEXT | L, split after U.0 |

Two evidence-machinery repairs ride along as standalone commits, each
with its own measured rationale:

- `make bench` / `make perf-baseline` is broken at this pin:
  `lisp-arithmetic-loop`'s `lisp_gc_count > 1` (Phase 21's Finding 6
  repair) is unreachable in a 440101-cell arena.  Second time a baseline
  moved under an assertion in that file; the repair must hold the
  instrument's conditions fixed (the gc-stress lane's `ARENA_BYTES`
  precedent) rather than loosen the assertion.
- `src/lisp_core.c` cites `reachable_live_objects` 9336; the census now
  says 9633.  Prose the ratchet cannot see; one line.

## Waves

**U.0 -- freeze (entry gate).**  For every backlog item: Emacs
31.0.91's exact behavior measured FIRST and frozen as oracle cases
(runner-produced snapshots, fresh case names); and the demand map
measured, not remembered -- for items 1-5 the census's plain-kgbatch
first-blocker method re-run on the named consumers to record what each
package advances TO once its blocker falls (the next blocker is the
backlog's next row, or its absence is the item DONE).  For item 6 the
map is the deliverable: which cl-lib names the 35 packages actually
reach (function position, the forecast-audit method over their sources),
ranked, so the subset is sized by demand rather than by cl-lib's index.
No implementation in U.0.

**U.1 -- prelude and kg-side items** (1, 3, and 6's first slice per
U.0's ranking).  Original code, Emacs-shaped, measured against the
oracle; never ported from GPL sources (the external/ quarantine gate
stays: nothing under external/ reaches shipped artifacts).

**U.2 -- the engine and fe chain** (2, 4, 5), fe-first pin discipline,
tiny-regex-c first for item 5, every expectation measured on Emacs
before code.

**U.3 -- exit.**  The census re-run whole: the loadable-package count
(today: exactly 1 of 110) and the first-blocker histogram are the
phase's before/after; the full 16-step matrix at the head; results
written into this document.

## Ground rules carried forward

Ratchet/census moves carry rationale and measured proof in commit
messages; oracle flips are expect/status edits proved by re-running the
gate; XPASS fails; `make format-check` runs beside `make check` in
every wave (R2b's lesson); a prelude edit runs `make
lisp-prelude-generate` by hand (same lesson, same wave).

## U.0 results -- the freeze

Done on 2026-08-20 at kg `7d83f78` / fe `e1d4fbd`, against GNU Emacs
31.0.91 (`/opt-3/emacs-31-lucid/bin/emacs`), the same oracle the corpus
is pinned to.  **No backlog item was implemented.**  What landed is 41
oracle cases and 8 manifest rows in `test/lisp-compat/`, and this
section.  Every ELPA file was read IN PLACE under `/root/.emacs.d/elpa`;
the shims and probes live in the session scratchpad and are named where
they are used.

### 1. What was frozen, item by item

41 new cases under `test/lisp-compat/cases/u0-*.json`, snapshots
produced by `fe/utils/run-emacs-oracle.py` (the recorder refuses a
re-pin, so every snapshot carries 31.0.91's own banner), 8 rows in
`test/lisp-compat/features.json`.  **31 are recorded divergences and 10
are agreements** -- the F.0 ratio and the F.0 reason: an agreeing
control is what makes the divergence beside it attributable.

| Item | Row | Cases | Emacs' verdict, in one line |
| --- | --- | ---: | --- |
| 1 | `u0-defalias-docstring` | 5 | The third argument is STORAGE: it lands on `function-documentation`, `documentation` reads it back, a symbol target carries the alias's own docstring, a non-string is accepted verbatim, and a fourth argument is `wrong-number-of-arguments (defalias 4)`. |
| 1 | `u0-require-arity` | 4 | NOERROR non-nil returns nil for a missing feature and provides nothing; NOERROR nil raises `file-missing` with kg's own data shape; all three arities return the feature symbol for a feature already provided. **kg's `require` already takes FILENAME** -- the gap is one slot. |
| 1 | `u0-regexp-opt-paren` | 7 | nil is a SHY group; `t` is a CAPTURING group, so the whole match is also group 1 (a numbering shift on the caller); `words` adds `\<`/`\>` and `symbols` adds `\_<`/`\_>`, both zero-width, both changing what matches; a STRING is the literal group opener (`"\(?2:"` -> group 2); any other non-nil value behaves as `t`; the empty member list stays unmatchable. |
| 2 | `u0-reader-unknown-escape` | 6 | An UNKNOWN string escape reads as the character itself (`"a\(b)"` -> `"a(b)"`), over six escapes -- except backslash-SPACE, which reads as NOTHING (the string-continuation escape), and except the escapes the reader already knows, which keep their meanings. |
| 3 | `u0-subr-x` | 4 | `(require 'subr-x)` really loads (`featurep` is nil before it) and returns `subr-x`; `string-blank-p` answers a MATCH POSITION rather than `t`; `thread-first`/`thread-last` differ in sign on the same steps; three of the four most-reached names are already kg's. |
| 4 | `u0-define-error` | 5 | `define-error` is two `put`s: conditions are the symbol consed onto the parent's list, read at DEFINE time; a list of parents is appended in order; an unknown parent yields conditions WITHOUT `error`, so `t` catches and `error` does not; `error-message-string` renders "MESSAGE: datum, datum" and "peculiar error: ..." for an undeclared symbol. |
| 4 | `u0-signal-error-conditions-plist` | 4 | `signal` READS the plist, so a handler naming any middle member catches; Emacs' own built-in conditions are readable plists (`(get 'wrong-type-argument 'error-conditions)` is `(wrong-type-argument error)`, kg's is nil); **an UNDECLARED symbol answers exactly what kg answers.** |
| 5 | `u0-regexp-contextual-anchors` | 6 | `$` anchors only at the end of the regexp or before `\|` or `\)`, `^` only at the start or after `\(`, `\(?:` or `\|`; anywhere else each is an ORDINARY CHARACTER (`"a$b"` matches the text `a$b`, `"a$*"` matches `a$$`). The s-lex-format pattern `"${\([^}]+\)}"` matches `x ${abc} y` at 2..8 with group 1 `"abc"`. |

The ten agreements, one per contract, are
`u0-defalias-two-argument-baseline`, `u0-require-two-argument-baseline`,
`u0-regexp-opt-paren-nil-is-shy`, `u0-reader-known-escapes-baseline`,
`u0-subr-x-names-kg-already-has`,
`u0-signal-undeclared-symbol-baseline`,
`u0-condition-hierarchy-baseline`,
`u0-anchor-dollar-anchors-at-the-end-and-before-alternation`,
`u0-anchor-caret-anchors-at-the-start-and-after-alternation` and
`u0-anchor-escaped-dollar-baseline`.  One case carries
`expect: diverge` on purpose: `u0-reader-hex-escape-is-greedy` records a
divergence item 2 must NOT close (both readers already agree that `\x`
eats hex digits greedily; kg's refusal there belongs to the unibyte-string
row).

Oracle gate, before and after:

    before   441 comparison=emacs case(s), 399 passed, 42 divergence(s), 0 failed
    after    482 comparison=emacs case(s), 409 passed, 73 divergence(s), 0 failed

### 2. The census probe, reproduced before it was reused

Phase 28's plain-`kgbatch` first-blocker census was re-run whole before
any demand map was built on it, because a demand map is only worth what
the probe under it is worth.  **The plain column reproduces exactly**:
110 packages, 91 `require` of an absent library, 9 `eval-when-compile`,
2 `defgroup`, one each of `expand-file-name`, `file-name-sans-extension`,
`make-syntax-table`, `declare-function`, `eval-and-compile`,
`require`/3, the unknown escape, and 1 package (`s`) loading to
completion.

**The shimmed column reproduces exactly too -- but only with one
correction to how the shim is described.**  Phase 28 §6.3 calls its shim
`eval-when-compile`, `eval-and-compile`, `declare-function`, `defgroup`,
`defcustom`, `defface` and `gv-define-setter` "as inert macros".  Inert
`eval-when-compile`/`eval-and-compile` do NOT reproduce the published
numbers: they give 91/7/2/... and carry four packages (`loc-changes`,
`spinner`, `shift-number`, `typescript-mode`) past a `require` they
should have stopped at.  Making those two EVALUATE their bodies -- which
is what Emacs does when it interprets them -- reproduces
95/7/1/1/1/1/1/1/1/1 = 110, the published column, class for class.  The
other five are genuinely inert.  Recorded here so the next re-run of this
probe reproduces rather than re-derives.

### 3. Demand maps: what each item's consumers advance TO

Method: the census's own probe, with a MINIMAL scratchpad stand-in for
the item under test layered on the reproduced census shim, and the named
consumers re-probed.  A stand-in is the smallest thing that makes the
blocker go away and nothing else.

#### 3.1 Items 1 and 2 are ONE CHAIN, not two rows

All seven `defalias`/3 consumers (`ansible`, `cfrs`, `dash`,
`dired-hacks-utils`, `f`, `ht`, `treemacs`) block inside the same file,
`dash.el`, and with the docstring argument stood in for they advance to
**`dash.el:822` -- backlog item 2, the `\(fn ...)` reader escape, in the
same file.**  Standing in for that as well (a scratchpad copy of dash.el
with `\(` rewritten to `(`, which is what Emacs' reader produces),
the chain continues:

| # | dash.el | blocker | what it is |
| ---: | ---: | --- | --- |
| 1 | :603 | `Wrong number of arguments: defalias, 3` | backlog item 1 |
| 2 | :822 | `unsupported read syntax: unknown escape` | backlog item 2 |
| 3 | :1067 | `void-variable lexical-binding`, and with it bound: `unsupported feature: eval lexical argument` | **a DORMANT branch** -- see §4.2 |
| 4 | :3966 | `void-function rx` | three calls, all building font-lock keywords |
| 5 | :3966 | `void-variable emacs-major-version` | |
| 6 | :4096 | `void-function define-minor-mode` | `dash-fontify-mode` |
| 7 | :4137 | `void-function make-obsolete-variable` | |
| 8 | -- | **dash.el LOADS** | 4177 lines, all of it |

With all seven stood in for, the six consumers advance PAST dash into
their own next blockers: `ansible` and `f` to `f.el:43 void-function
version<=`, `cfrs` to `(require 'posframe)`, `dired-hacks-utils` to
`(require 'dired)`, `ht` to `(require 'gv)`, `treemacs` to
`(require 'treemacs-macros)`.

**The correction this forces**: the backlog's "7 packages via
dash.el:603" is the number of packages that STOP there, not the number
items 1 and 2 unblock.  Items 1+2 are steps 1 and 2 of a seven-step
chain in one file, and nothing in the census's loadable count moves
until the rest of it does.  They are still worth doing first -- they are
the two cheapest steps and step 3 is decided by them -- but the
before/after U.3 will measure is `dash.el`'s blocker LINE moving, not
the package count.

#### 3.2 Item 1's other two consumers

* `require`/3 (`cython-mode`, the census's one): with NOERROR honoured,
  it advances to `(require 'python)` -- an absent library, the census's
  largest class.  The item closes a signature, not a package.
* `regexp-opt`/2 (`yaml-mode`, the census's one): with PAREN accepted,
  it advances to `yaml-mode.el:188 void-function make-sparse-keymap`.

Call sites across the whole ELPA tree, counted by parsing rather than by
grepping (`utils/forecast_audit.py`'s reader, imported and not
modified): `(defalias ...)` with three or more arguments, **29 sites in
17 packages**; `(require ...)` with three, **80 sites in 34 packages**;
`(regexp-opt ...)` with two, **69 sites in 16 packages**.

#### 3.3 Item 3 (`subr-x`): the feature is the blocker, and the demand is 13

A stand-in that does nothing but `(provide 'subr-x)` advances all four
of today's blocked packages, and **not one of them then stops on a
`subr-x` name**: `elfeed` -> `(require 'xml-query)`, `simple-httpd` ->
`(require 'cl-lib)`, `xterm-color` -> `(require 'cl-lib)`, `yaml` ->
`(require 'seq)`.

The item is also bigger than the census's 4.  With `cl-lib` and `compat`
provided as bare features (§3.5), **nine more packages reach this same
require**: `consult`, `rmsbolt`, `cape`, `corfu`, `eat`, `embark`,
`jinx`, `marginalia`, `vertico`.  Demand 13, second only to `cl-lib`.

The name tail behind the feature, over those 13 packages' sources,
restricted to the 31 names Emacs 31 actually defines in `subr-x`:

| pkgs (of 13) | refs | name | kg has it |
| ---: | ---: | --- | --- |
| 11 | 58 | `string-join` | yes |
| 5 | 16 | `string-blank-p` | no |
| 3 | 13 | `string-remove-prefix` | no |
| 3 | 4 | `thread-last` | no |
| 3 | 3 | `string-remove-suffix` | no |
| 2 | 2 | `string-clean-whitespace` | no |
| 1 | 4 | `hash-table-values` / `-empty-p` / `-keys` (one package) | no -- the dormant hash-table branch, not this item |
| 1 | 1 | `named-let` | no |
| 1 | 1 | `thread-first` | no |

#### 3.4 Items 4 and 5

* **Item 4** has no first blocker anywhere in the census and is measured
  in call sites instead, which U.0 says plainly rather than inventing a
  load probe: **12 packages call `define-error`, 66 times**; **5 more
  declare or read conditions through the plist directly** (`s.el:627`,
  `f.el:47`, `websocket.el` x7, `emacsql-compiler.el:28`,
  `ellama.el:3607`).  The measured consumer is s.el's own error path:
  `(s-format "${a}" 'aget nil)` answers `(s-format-resolve "${a}")` in
  Emacs and `(error ("Invalid error symbol" s-format-resolve))` in kg.
  **A `define-error` alone does not fix it** -- s.el never calls
  `define-error`; it writes the plist with `put`.  The two halves are one
  item.
* **Item 5** has exactly one measured consumer and the item CLOSES it.
  With the one pattern `s-lex-fmt|expand` matches rewritten to a
  character class kg's engine reads, `(let ((a 5)) (s-lex-format
  "${a}"))` answers `"5"` -- Emacs' own answer -- and a two-variable
  template answers `"n=x v=2"`.  There is no next blocker to record.
  Breadth, for whoever sizes the engine work: among ELPA string literals
  that are certainly regexps, a `$` in a literal position by Emacs' rule
  appears in 19 files across 14 packages, and a literal `^` in 17 files
  across 14 packages.

#### 3.5 Item 6 (`cl-lib`): the map IS the deliverable, and it has two halves

**Half one, measured at LOAD time.**  A stand-in that does nothing but
`(provide 'cl-lib)` (and `(provide 'compat)`) advances all 47 packages,
and **only two of the 35 then stop on a `cl-` name**: `llm` at
`cl-defstruct` and `zmq` at `cl-deftype`.  One package,
`inheritenv`, LOADS TO COMPLETION.  The rest stop somewhere else
entirely:

| where the 35 cl-lib packages stop once the feature exists | count |
| --- | ---: |
| another absent library (26 packages, 19 distinct libraries; `cond-let` 3, `subr-x` 2, `rx` 2, `eieio` 2, `ring` 2, `compile` 2, then singletons) | 26 |
| a missing editor-surface name (`make-variable-buffer-local` x3, `defvar-local`, `getenv`) | 5 |
| a `cl-` name (`llm`: `cl-defstruct`; `zmq`: `cl-deftype`) | 2 |
| the unknown reader escape (`lv`) | 1 |
| **loads to completion** (`inheritenv`) | 1 |

The 12 `compat` packages behave the same way: 7 go straight to
`(require 'subr-x)`, the others to `comint`, `gptel`, `cond-let`,
`define-advice` and `defsubst`.  Not one stops on a `cl-` name.

Whole-tree effect, with `cl-lib`, `compat` and `subr-x` provided as bare
features on top of the census shim, all 110 packages re-probed:

| first blocker | census | with the three features |
| --- | ---: | ---: |
| `require` of an absent library | 95 | 78 |
| `Wrong number of arguments: defalias, 3` | 7 | 8 |
| `void-function` / `void-variable` (13 distinct names) | 4 | 17 |
| `unsupported read syntax` | 1 | 3 |
| `Wrong number of arguments: require, 3` / `regexp-opt, 2` | 2 | 2 |
| **loads to completion** | 1 | 2 |

The residue is a LONG TAIL: `map` 4, `cond-let` 4, `eieio` 3, `compile`
3, `gptel` 3, `rx` 3, then twos and ones.

**Half two, the source census**, which is what a package needs to RUN
rather than to load.  Collected with `utils/forecast_audit.py`'s own
reader and walker -- imported, not modified -- over every `.el` file of
each package (autoloads and `-pkg.el` excluded), counting names in
function position exactly as the tracked audit counts them: 124 files
for the 35 `cl-lib` packages (0 unreadable), 48 for the 12 `compat`
ones.  **86 distinct `cl-` names, 912 references** in the first group;
94 names and 1357 references over all 47.  Ranked by how many packages
reach the name:

| pkgs (of 35) | refs | name |
| ---: | ---: | --- |
| 15 | 105 | `cl-loop` |
| 13 | 66 | `cl-defun` |
| 12 | 30 | `cl-incf` |
| 10 | 23 | `cl-case` |
| 9 | 14 | `cl-find-if` |
| 8 | 47 | `cl-defstruct` |
| 7 | 259 | `cl-defmethod` |
| 7 | 65 | `cl-defgeneric` |
| 6 | 17 | `cl-destructuring-bind` |
| 6 | 14 | `cl-labels` |
| 6 | 14 | `cl-some` |
| 6 | 12 | `cl-assert` |
| 6 | 10 | `cl-remove-if-not` |
| 5 | 14 | `cl-mapcan` |
| 5 | 7 | `cl-flet` |
| 5 | 6 | `cl-position` |
| 4 | 12 | `cl-pushnew` |
| 4 | 11 | `cl-decf` |
| 4 | 6 | `cl-reduce` |
| 4 | 6 | `cl-remove-if` |

Over all 47 packages the head is the same and the counts roughly double:
`cl-loop` 25 packages / 237 refs, `cl-defun` 15/89, `cl-incf` 14/85,
`cl-find-if` 12/19, `cl-defmethod` 11/303, `cl-defgeneric` 10/82,
`cl-case` 10/23, `cl-defstruct` 9/51, `cl-assert` 8/20,
`cl-remove-if-not` 8/17, `cl-letf` 8/12.

**What the two halves say together, and it is not what the backlog row
assumed.**  The row reads "35 packages stop at `(require 'cl-lib)` ...
U.0 must first measure what those packages reach for NEXT", and the
answer is: at load time, almost nothing from `cl-lib` -- the FEATURE is
the blocker and the names are a runtime surface behind it.  So a first
slice sized by this ranking (`cl-loop`, `cl-defun`, `cl-incf`,
`cl-case`, `cl-find-if`) does not by itself move the loadable count;
what moves it is `provide`, and `provide` without the names moves the
failure from load time to run time.  That is a policy question -- how
much of `cl-lib` kg claims when it answers `(require 'cl-lib)` -- and it
is the same question Phase 28's Branch 3 said had to be stated before
advertising `cl-defstruct`.  **U.0 states the numbers and does not
decide it.**  Two facts bound the decision: `cl-defstruct` is 8 of the
35 packages and is the only `cl-` name that is a first blocker anywhere
(with `cl-deftype`), and it needs records, which is Branch 2.

### 4. Corrections U.0's measurements force on this plan

Stated plainly, as findings, on F.0's precedent.

**4.1 The backlog's demand column counts STOPS, not unblocks.**  Items 1
and 2 are steps 1 and 2 of a seven-step chain inside `dash.el` (§3.1);
item 1's `require`/3 and `regexp-opt`/2 consumers each advance to
something else; item 3's four consumers advance to four other absent
libraries.  Only item 5 CLOSES its want outright.  The phase's exit
measurement (U.3) should therefore report the blocker LINE each package
reaches as well as the loadable count, or it will read as no progress.

**4.2 A dormant branch has just been named -- and U.0 also measured the
way around it.**  With items 1 and 2 stood in for, six packages reach
`dash.el:1067`, which is `(eval condition lexical-binding)` inside
dash's `static-if` polyfill.  That is Phase 28's Branch 1 (first-class
lexical environments), adjudicated dormant because "no package's first
blocker is either refusal" -- and now one is, two steps in.  With
`lexical-binding` bound to `t` kg answers its own named refusal,
`unsupported feature: eval lexical argument`.  **But the polyfill is
guarded by `(unless (fboundp 'static-if) ...)`**: Emacs 30 and later
have `static-if` built in, and a kg that DEFINES `static-if` never
reaches the `eval` call at all -- measured, the chain then advances
straight to step 4.  So the cheap answer is a name, not a branch, and
Branch 1 stays dormant on this evidence.  (Binding `lexical-binding` to
`nil` has the same effect and is worse: Emacs answers `t` for a package
file, so kg would be lying about the dialect to skip a code path.)

**4.3 Item 4's evidence line is half wrong in the census, and the two
halves of the item cannot be split.**  Phase 28 §6.2's exhibit --
kg `(error ("Invalid error symbol" my-err))` versus Emacs `(my-err
nil)` -- is only a divergence because the Emacs side had a `put` in
front of it.  Measured without it, Emacs answers exactly what kg answers
(`u0-signal-undeclared-symbol-baseline`, an agreement).  And a
`define-error` that only creates plists fixes nothing on its own,
because `s.el`, `f.el`, `websocket.el` and `emacsql` never call
`define-error`: they write the plist with `put` and signal it.  The item
is `signal` reading the plist, with `define-error` as the convenience on
top.

**4.4 Item 1's `regexp-opt` half is two costs, not one.**  PAREN nil,
`t` and a STRING are a wrapper around what kg already produces.  PAREN
`words` and `symbols` are `\<`/`\>` and `\_<`/`\_>`, which kg's engine
does not have -- the same engine gap `frontier-regexp-shy-group`
recorded before R2 closed it.  An S-sized item covers three of the five
values; the other two are engine work in item 5's chain
(tiny-regex-c -> fe -> kg).

**4.5 The `regexp-opt` PAREN decision is a REVERSAL and has to be
written as one.**  `frontier-regexp-opt-paren-shapes` (F.0) records the
opposite decision -- a second argument is a named arity error, on the
Phase 14 `intern` precedent -- with its rationale.  The census named a
consumer, which is exactly how this campaign says a closed question
reopens; but U.1 must edit that case's note when it lands, or the corpus
will hold two contradictory decisions about one name.

**4.6 Item 3 is under-ranked in the backlog table.**  Its demand is 13
packages, not 4, once `cl-lib` and `compat` are provided -- and since
the census's own measurement is that `cl-lib` and `compat` are
*feature* blockers (§3.5), that is a near-certain ordering rather than a
hypothetical.  Its cost is also the smallest of the six: a `provide`
plus five names, three of which kg already has.

**4.7 The census shim's description needs one word changed** (§2):
`eval-when-compile` and `eval-and-compile` must evaluate their bodies to
reproduce the published shimmed column.  Nothing about Phase 28's
conclusions changes; the reproduction instructions do.

### 5. Verified

    $ git rev-parse HEAD                    7d83f78 (freeze commit's parent)
    $ git -C fe rev-parse HEAD              e1d4fbd
    $ make -j8 check                        EXIT=0
                                            unit 59/59 PASS
                                            pty  594: 588 PASS, 6 SKIP, 0 FAIL, 0 XPASS
    $ make format-check                     EXIT=0
    $ make lisp-compat-check                616 feature(s), 0 problem(s)
    $ make lisp-oracle-check                482 cases, 409 passed,
                                            73 recorded divergences, 0 failed
    $ python3 fe/utils/run-emacs-oracle.py test/lisp-compat --emacs /opt-3/...
                                            41 written/updated, 0 failed
    $ ./test/kgbatch <110 ELPA main files>  plain and shimmed columns of
                                            Phase 28 section 6.3 reproduced

`/proc/loadavg` was 6.3 .. 10.5 across the run; nothing here is timed,
and the two gates that could be load-sensitive (the PTY layer, the
oracle runner) are pass/fail on saved-file and record comparisons.  No
ratchet moved and none was re-baselined.  Nothing under
`/root/.emacs.d/elpa/` was copied into the tree, modified, or committed;
the census shim, the per-item stand-ins and the scratchpad copy of
`dash.el` used to see past the reader escape all lived in the session
scratchpad.
