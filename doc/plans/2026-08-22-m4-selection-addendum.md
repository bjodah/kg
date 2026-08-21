# Phase M4 — C2 package vertical selection addendum

Date: 2026-08-22.  Phase M4 is **selection only**; no implementation of any
package vertical may start here (master plan §8, hard rule).  This document
is the selection deliverable: scored candidate set, per-candidate closure,
a stop budget, and an explicit owner decision.

## 0. State at the entry point (cite)

* **C1 published** — `s.el` is scenario-green for `s-core/ascii` (kg commit
  `81c4d84`).  The M1 runner (`test/elisp-packages/` + `utils/check_elisp_packages.py`)
  and the `s-core/ascii` label are in `README.md` / `doc/lisp-api.md`.
* **Phase M3 landed** — every non-supported `test/lisp-compat/features.json`
  row carries a `failure_mode` (kg `d07ce92`).  Measured dashboard on
  `test/lisp-compat/features.json` today: `silent-wrong:36`,
  `loud-unsupported:10`, `intentional-policy:9`, `resource-bound:1`.  (The
  plan text references the d07ce92 count `loud-unsupported:11`; the single
  row difference is expected drift since that commit and is immaterial to
  this selection.)
* **Reader unknown-escape semantic landed** — FE_LANGUAGE_VERSION 19→20,
  FeVersion "24.0" (kg commit `1ffc762`, fe `82a29b7`).  The U.0 unknown-escape
  and string-continuation cases flipped to `agree`, so dash's two
  reader-escape blockers (chain steps 2 and 3) are **gone**.

Fresh measurements run for this addendum (kgbatch against the current
build):

| probe | result | meaning |
| --- | --- | --- |
| `(fboundp 'string-blank-p)` | `t` | subr-x string helpers present, unadvertised |
| `(string-blank-p "  ")` | `0` | **still silent-wrong**: Emacs returns `t`, kg returns a match position — the `u0-subr-x` divergence is live |
| `(fboundp 'named-let)` | `nil` | not implemented |
| `(fboundp 'hash-table-keys)` | `nil` | Fe has **no hash-table type** (TODO watch item; `fe/compat` short-circuits `hash-tables` as unsupported) |
| `(require 'subr-x)` | `file-missing` | feature retracted (M0.1), honest diagnostic |
| editor-surface `fboundp`: `global-set-key`/`define-key`/`kbd`/`defcustom`/`make-obsolete-variable` | `t` | partially present |
| `define-minor-mode`/`define-obsolete-function-alias`/`rx`/`named-let`/`cl-letf`/`gv-define-setter` | `nil` | missing |

## 1. The six-factor scorecard

Candidates scored (master plan §8 mandates at least `dash`, completing
`subr-x`, `inheritenv`, plus one small editor utility).  Each factor rated
H/M/L (high/medium/low contribution or risk).

| candidate | user value | scenario quality (no fetch) | dependency closure | semantic leverage | failure risk | ongoing cost |
| --- | --- | --- | --- | --- | --- | --- |
| `dash` | H (big lib) | H (pure list API) | **L** (very wide) | M (macro/regexp) | H (silent misbind) | M |
| complete `subr-x` | L (names already callable) | H | **L** (hash tables) | L (few names) | M (string-blank-p silent-wrong) | L |
| `inheritenv` | M | M | **L** (cl-letf*/advice/proc) | L | H | M |
| small editor util (e.g. `string-inflection`) | M | H | M (point/word + interactive) | M | M | L |

`inheritenv` is **not** scored up on package count: it loaded only because
`(require 'cl-lib)` was (falsely) provided; with `cl-lib` retracted it stops
at the require again, and its useful path needs `cl-letf*`, generalized
variables, process/environment variables and advice — exactly as the plan
pre-warns.  Its demand is one package; user value does not rescue the
closure.

## 2. Per-candidate detail

### 2.1 `dash` — DEFER (closure exceeds C2 budget)

* **provenance**: upstream `magnars/dash.el`; the 110-package census used
  the ELPA tree at `/root/.emacs.d/elpa` pinned to GNU Emacs 31.0.91
  (doc `2026-08-20-elisp-demand-first-unblock.md` §3.1, §6.4).  **No
  vendored copy exists** in `external/elpa/` (only `s.el` is tracked), so
  this is scored from the census + U.1a chain map, not a fresh load.
* **reader change impact**: commit `1ffc762` removes dash chain steps 2
  (`\(fn ...)` in a string) and 3 (`?\(` in a char literal).  Remaining
  chain (post-U.1a, `unblock.md` §6.4):
  1. `:46` `(require 'cl)` behind `(fboundp 'gv-define-setter)` — one inert
     macro (`gv-define-setter`, **S**, kg)
  2. *removed*
  3. *removed*
  4. `:3966` `void-function rx` — **rx** is a regexp-DSL macro expanding to
     tiny-regex-c patterns; **L**, owned fe/kg (tiny-regex-c already lands
     the anchors it would use)
  5. `:4096` `void-function define-minor-mode` (`dash-fontify-mode`) —
     editor-definition surface, **M**, kg (project 9.3)
  6. `:4127` `void-function define-globalized-minor-mode` — **M**, kg
  7. `:4130` `defcustom :set` keyword unsupported — **S**, kg
  8. `:4140` `void-function define-obsolete-function-alias` — **S**, kg
  9. loads
* **representative scenarios to freeze** (post-load, from Emacs 31.0.91):
  `(-map #'1+ '(1 2 3))`→`(2 3 4)`; `(-filter #'evenp ...)`;
  `(-reduce #'+ '(1 2 3))`→6; `(-flatten '((1) (2 3)))`;
  `(-partition 2 ...)`; `(-zip-with #'+ ...)`; `(-group-by #'evenp ...)`;
  `dash-fontify-mode` toggles font-lock keywords.
* **transitive closure**: rx (L, fe/kg), minor-mode + globalized-minor-mode
  + defcustom `:set` + define-obsolete-function-alias (M+S, kg).  None of
  these are C2-sized; rx alone is an L substrate project (project 9.x-range
  regexp DSL), and the editor-definition surface is its own unmeasured
  substrate.
* **verdict**: trips stop condition "a standard feature cannot be completed
  within the declared phase budget" (rx L + editor surface M).  **DEFER.**

### 2.2 Completing `subr-x` — DEFER (cannot honestly advertise; low value)

* **provenance**: Emacs standard library, not a third-party package.  The
  feature was provided then **retracted** (M0.1 / `doc/lisp-api.md`
  "Features kg provides without a file"); the working names live
  unadvertised in `lisp/prelude.el`.
* **what kg has today** (fresh probe): `string-blank-p` (returns `0`,
  silent-wrong), `string-remove-prefix/-suffix`, `string-pad`,
  `string-clean-whitespace`, `thread-first/-last` — all fboundp, all
  unadvertised.  Missing: `named-let` (nil) and the four `hash-table-*`
  (nil; **no hash-table type in Fe at all**).
* **representative scenarios**:
  `(string-blank-p " ")`→`t` (fix the live divergence);
  `(string-remove-prefix "a" "ab")`→`"b"`;
  `(thread-first 5 - (* 2))`→`-10`;
  `(named-let loop ((x 1)) (if (> x 3) x (loop (1+ x))))`→`4`.
* **transitive closure / missing families**:
  * `named-let` — lexical self-recursion, **M**, fe/kg (needs a
    self-referential closure; fe evaluator is lexical but `cl-labels`-style
    named recursion is unmeasured — a possible second semantics, a stop
    condition to watch).
  * `hash-table-*` — **L**, **fe** (new data type + reader syntax +
    primitives).  Off the roadmap (TODO: "four hash-table names … stay off
    the roadmap").
  * `string-blank-p` silent-wrong — **S**, kg prelude fix.
* **the §3.2 problem**: `(provide 'subr-x)` is a capability promise
  `require`/`featurep` can see.  Without hash tables the contract is
  incomplete and no version/capability mechanism exists to select the
  implemented subset, so re-providing would be the same false-capability
  shape that already caused `cl-lib` and `subr-x` retractions.  The only
  honest completion is `named-let` + the `string-blank-p` fix, left
  **unadvertised** — which is the current state plus two names.  That
  unblocks **zero** of the 13 demand packages (they need the feature bit
  *and* their own next blockers).  High effort-measurement, near-zero
  product movement.  **DEFER.**

### 2.3 `inheritenv` — DEFER (largest unmet closure of the mandated set)

* **provenance**: GNU ELPA `inheritenv`; **not vendored** in
  `external/elpa/`, scored from the census.  It "loaded" only under the
  retracted `(provide 'cl-lib)`; with `cl-lib` retracted it stops at
  `(require 'cl-lib)` again (unblock.md §6.2, external-review tranche).
* **useful path needs**: `cl-letf*` (generalized variables, project 9.2,
  **L**), process/environment-variable get-set primitives (**M**, kg), and
  advice semantics (project 9.3, **M**).
* **verdict**: trips "requires broad `cl-lib`/… or a second semantics"
  stop condition.  Lowest value-per-closure of the set.  **DEFER.**

### 2.4 A small editor utility — profiled, DEFER (unmeasured closure)

Picked as the *most plausibly C2-bounded* real workflow utility for a
terminal code editor: a keybound string-inflection command
(`camelCase`↔`snake_case`↔`UPPER_CASE`), e.g. the shape of
`aki2o/string-inflection`.  **No source vendored**; profiled from capability
reasoning + the fboundp census above.

* **representative scenarios** (pure-string, freezable without fetch):
  `(string-inflection-camelcase "foo_bar")`→`"fooBar"`;
  `(string-inflection-underscore "fooBar")`→`"foo_bar"`;
  `(string-inflection-upcase "fooBar")`→`"FOO_BAR"`; a PTY case asserting
  the bound key cycles the inflection of the symbol at point and leaves
  point/saved-state correct.
* **transitive closure / missing families**:
  * point/word detection — `thing-at-point` / word-at-point is **nil**
    (fresh probe); kg has word syntax but no point-anchored word/sexp
    extractor.  **M**, kg.
  * end-to-end `interactive` + `defcustom` + `global-set-key`/`kbd` driving a
    real keybinding and command — partially fboundp but **unmeasured**
    end-to-end (project 9.3 territory).  **S–M**, kg.
  * No *new standard FEATURE name* is required; the closure is
    editor-surface **correctness**, not a new semantic family.
* **verdict**: best closure/budget ratio of the four, but its true closure
  is **unmeasured** and a wrong point/region edit is exactly the
  "silent misbinding / wrong editor state" the stop conditions guard
  against.  The mature move is to run a cheap census-style closure probe
  (the fboundp pass above is a start) and only then decide.  For M4,
  **DEFER** — but record it as the leading future candidate once its
  point/word extraction is measured.

## 3. Stop budget

C2's implementation envelope is decided *after* a vertical is chosen; M4's
own cost is **S** (this document).  The budget M4 imposes on any future C2:

* **No new data type.**  Hash tables (Fe) are out of scope per the roadmap;
  any vertical needing them is rejected at selection (kills `subr-x`
  honesty and any hash-table-dependent consumer).
* **No single new semantic family larger than M.**  `rx` (L), generalized
  variables / `cl-letf*` (L), and advice (M+) are each individually
  disqualifying for C2; they belong to substrate projects 9.1–9.3, not to
  a package vertical.
* **Max two M-sized standard-feature items** of honest, measured closure
  per C2; everything else must already be fboundp-and-verified.
* **Zero C1 regressions** (stop condition) and **no ratchet rise without a
  local, measured explanation** (repo rule) — both non-negotiable gates.
* **Scenario quality gate**: a C2 vertical must be freezable without fetch
  or external services (all four candidates pass this; it is not the
  differentiator).

Every candidate above trips at least one of these lines.  That is the
budget doing its job.

## 4. Owner decision: **DEFER**

Per master plan §11 ("The acceptable outcome of an M4 selection is
**defer**") and the explicit instruction not to let enthusiasm pick a
vertical the stop conditions would kill, the decision is **DEFER** — do not
start any C2 vertical in M4.

Decisive reasons, from the six factors and the stop conditions:

1. **Closure exceeds budget for every mandated candidate.**  `dash` needs
   `rx` (L) + the editor-definition surface (M); `inheritenv` needs
   `cl-letf*`/advice/process vars (L+M); `subr-x` cannot be honestly
   advertised without hash tables (L, off-roadmap) and its only real
   addition (named-let + a `string-blank-p` fix) unblocks no package.
   Each trips "a standard feature cannot be completed within the declared
   phase budget."
2. **The leading small utility is unmeasured, not unviable.**  A
   keybound string-inflection utility has the smallest closure, but its
   point/word detection (`thing-at-point` = nil) and end-to-end
   `interactive`/`global-set-key` path are unverified; committing C2 now
   would risk a silent wrong-editor-state scenario — the exact failure the
   stop conditions exist to prevent.  The right next step is a cheap
   closure probe, not an implementation start.
3. **C1 is stable and must stay that way.**  Pushing a vertical whose
   closure is mostly substrate work would pull M4 past "selection only,"
   risk C1 regress, and manufacture `loud-unsupported` noise on the
   failure-mode dashboard (currently `silent-wrong:36 /
   loud-unsupported:10 / intentional-policy:9 / resource-bound:1`) without
   buying a scenario-green package.

A stable, truthful C1 library profile is more mature than a large package
surface that fails after `require` (master plan §11, closing sentence).
Re-run the broad census after a substrate project (editor-definition
surface, or point/word extraction) lands, and re-score `string-inflection`
first.

## 5. Source citations

* 110-package census & demand maps: `doc/plans/2026-08-20-elisp-demand-first-unblock.md`
  — §3.1 dash seven-step chain, §3.3 `subr-x` demand=13 (and its
  under-ranking correction §4.6), §3.5 `cl-lib`/`compat` map, §4.2 dormant
  lexical-env branch, §6.4 post-U.1a residual blockers (`defgroup` 7,
  `compat` 16, `rx`/`eieio`/`compile` each 3).
* Failure-mode dashboard: `test/lisp-compat/features.json` (measured
  `silent-wrong:36 / loud-unsupported:10 / intentional-policy:9`);
  plan-reference counts from kg `d07ce92`.
* `doc/TODO.md`: hash-table names "stay off the roadmap" (forecast-residual
  tail, ~line 440); `eval` LEXICAL gap; `defvaralias` indirection cost;
  backquote-printing block.
* `doc/lisp-api.md`: `(require 'subr-x)` and `(require 'cl-lib)` retracted
  (M0.1); `string-blank-p` returns a match position, not `t`.
* Fresh measurements this addendum: kgbatch probes (§0 table) — `string-blank-p`
  silent-wrong confirmed live; `named-let`/`hash-table-*` nil; `require 'subr-x`
  `file-missing`; editor-surface fboundp census.
* Reader escape landing: kg `1ffc762` (fe `82a29b7`, FE_LANGUAGE_VERSION 20)
  — removes dash chain steps 2 and 3.
* C1: kg `81c4d84` (`s-core/ascii` scenario-green).

This addendum changes no manifest and no test; it is a planning document,
not a support claim.  `README.md` / `doc/lisp-api.md` future-package
statements are intentionally **not** edited.
