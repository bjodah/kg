# 08A — Pin the init-file target, correct the parent, and fund the phase

Parent: [Phase 8](../2026-08-03-elisp-subset-and-fe-evaluator.md#12-phase-8--core-init-file-compatibility-roadmap).
No implementation. This slice freezes the measured Emacs answers the other
four slices build against, records where parent §12 is stale, and funds the
complexity the phase needs. The audit that grounds it ran 2026-08-06 against
kg at the Phase 7 close and Emacs 31.0.90 (`/opt-3/emacs-31-lucid/bin/emacs`,
`TERM=xterm-256color`); every number below must be re-measured at slice
start, not carried forward (Rule 6).

## The corpus: a real init file, run to the point of death

The audit's 30-line representative `init.el` (setq, setq-default, defvar,
defconst, two interactive defuns, global-set-key, add-hook, dolist, alist
lookup, mapc, mapconcat, custom-set-variables, a `?x` literal) fails on
**11 of 26 top-level forms** — and because load aborts at the first error,
`(load "init.el")` executes exactly two lines. The failures are all library
and reader gaps: `setq-default`, `add-to-list`, `kbd`, `assq`, `mapc`,
`mapconcat`, `identity`, `custom-set-variables`, `?x`, and a `require` that
cannot find a library kg itself ships. The parts Phase 7 built —
`(defun … (interactive) …)`, `global-set-key`, prefix delivery — all pass.
Check the experiment in a clone before pinning; the audit's harness
(`kgbatch`, a ~65-line main over `kg_lisp_eval_string()`) should become a
tracked test utility in this phase so every future question is a
one-second experiment.

## Measured answer tables to freeze (oracle cases in this slice)

Record each as a compat case + runner-produced snapshot (or a kg-policy row
where the batch oracle cannot compare). Never regenerate an existing
snapshot.

**Table C — constants and keywords**

| expr | Emacs 31.0.90 |
|---|---|
| `(setq t nil)` | `setting-constant t` (error; `t` unchanged) |
| `(setq nil 1)` | `setting-constant nil` |
| `(setq :kw 1)` | `setting-constant :kw` |
| `(let ((t 1)) t)` | `setting-constant t` |
| `(let ((nil 1)) 1)` | `setting-constant nil` |
| `((lambda (t) t) 1)` | `1` (a lambda parameter may shadow `t` under the pinned lexical oracle) |
| `:foo` | `:foo` (self-evaluating) |
| `(keywordp :foo)` / `(keywordp 'foo)` | `t` / `nil` |
| `(eq :a ':a)` | `t` |
| kg today | `(setq t nil)` → nil, then `(if t 1 2)` → 2: one assignment silently corrupts `if`/`cond`/`and`/`or` for the session |

**Table R — reader**

| input | Emacs | kg today |
|---|---|---|
| `?a` `?\n` `?\t` `?\e` `?\\` `?\s` `?\d` | 97 10 9 27 92 32 127 | symbol `?a` → void-variable |
| `?é` (UTF-8) | 233 | symbol |
| `?\C-a` `?\M-a` | 1, 134217825 | symbol |
| `#x10` `#o17` `#b101` `#xff` | 16 15 5 255 | symbols |
| `"\x41" "\101" "\e" "\d" "\s"` | `"A" "A" "\x1b" "\x7f" " "` | backslash silently dropped: `"x41"` … |
| `[1 2 3]` | a vector | three symbols `[1` `2` `3]` |
| `#:sym` | quoted/read result is the interned symbol `sym`; evaluating it signals `void-variable` | symbol named `#:sym` |
| `` `(1 . ,(+ 1 1)) `` | `(1 . 2)` | `(1 unquote (+ 1 1))` |
| nested backquote | correct depth | inner unquote evaluated at wrong depth |

Re-measure every row (especially the string-escape and modifier rows —
the audit spot-checked, this slice pins). The `?a` decision of 2026-07-28
is formally reversed here: its recorded rationale ("Fe has no character
type") died with Phase 5's `FeTInteger`, and the byte-at-a-time reader
argument does not survive inspection (UTF-8 lead bytes state their own
length; no lookahead is needed). The manifest row
`reader-char-literal-unsupported` retires in 08C.

**Table L — library** (per-function contracts; the audit verified the
existing ten agree, so pin only the seven absent ones plus the divergent
edges: `assq`, `mapc` (returns its LIST argument), `mapconcat` (3-arg;
separator optional in Emacs 29+ — measure 31's arity), `nreverse`
(mutation semantics), `delq`/`delete` (return value vs in-place, the
"assign the result back" contract), `add-to-list` (APPEND argument,
membership test is `equal`), `identity`, `prog2`, `max`/`min` (variadic,
`wrong-type-argument` on non-numbers), `(cond (5))` → 5, lone-string
`defun` body (Phase 7 review already restored), `(defvar x)` declares
without binding — Emacs `(boundp 'x)` → nil; decide record-as-divergence
or implement).

**Table D — diagnostics** (kg-policy rows, no Emacs comparison where the
shape is kg's own): `(load FILE)` error report names `FILE:LINE` with a
real line number; a runtime error in a loaded file carries a position;
`format` directives `%c` `%x` `%X` `%o` and field width/flags against
Emacs' `format` answers (measure `(format "%-5d|" 3)`, `(format "%05.2f" 1.5)`,
`(format "%c" 233)` — the last is a multibyte char, decide byte policy
consciously).

## Decisions this slice must take (with the measured evidence)

1. **Constant protection lives in fe core** (`setq`/`set`/`let` binding/
   `ArgsToEnv`), raising `setting-constant` — a new condition symbol under
   `error` in the static hierarchy. Keywords become self-evaluating at
   intern time (the `t` treatment in `FeMakeSymbol`). This is
   language-visible: `FE_LANGUAGE_VERSION` 6→7, and `FE_API_VERSION` only
   if the C API changes (expected: no).
2. **Reader policy: reject before implement.** 08C first makes `?…`,
   `#…` (except `#'`), `[`, and unknown string escapes *read errors*
   (satisfying §12's own "reject rather than misread" rule), then
   implements the subset with measured value: the shared escape table,
   `?` char literals (UTF-8 decode to codepoint integer, `\C-`/`\M-`
   modifiers per Table R), and `#x`/`#o`/`#b` radix integers. Vectors
   stay rejected (Wave E). Symbol escapes (`a\ b`) stay rejected —
   record the divergence.
3. **The prelude is the default home for library work.** Measured: scc
   counts neither `lisp/prelude.el` nor the generated `.inc`, and the
   audit's prototype of the entire Wave B remainder measured **+0 scc**;
   the only costs are `PRELUDE_DEFS` (bump the literal) and arena
   headroom (audit: +718 peak live slots, 1.3% of the arena — re-measure).
   New C natives need individual justification.
4. **`setq-default`/`setq-local` are documented aliases of `setq`** in a
   dialect with no buffer-local variables — absent is worse (it kills
   init files at line 3); a silent no-op is dishonest. Alias + recorded
   divergence.
5. **`load-path` stays a C array** this phase; `(add-to-list 'load-path …)`
   is recorded as unreachable-by-construction with `add-to-load-path` as
   kg's spelling. Re-examine only with corpus evidence.
6. **Source positions become honest**: `ReadEvaluatedFile` counts lines so
   `path:N` is a line, and `EvaluateInput` keeps a position across
   evaluation so runtime errors in loaded files carry one. fe-side, in
   08C (same files as the reader work).
7. **`defcustom` enters as a prelude macro in 08E** exactly per parent
   §12's contract (inert presentation keywords accepted, semantics-bearing
   keywords rejected, standard evaluated only when unbound), after its
   oracle cases are pinned here. It needs keywords (08B) first.

## Corrections to parent §12 (recorded, applied to the parent doc)

The audit measured eight; record them in the parent as a dated correction
block, as 07A did for §11:

1. Wave A's "move into Fe core" premise is inverted — eight of twelve
   items exist and agree with Emacs as prelude macros; only constant
   protection and keywords are core work.
2. The "old one-binding Fe `let`" bullet was satisfied before the plan
   was written (b37bb20, 2026-07-28).
3. Wave A under-weights its own highest-severity item: assignable `t`.
4. Wave B is ~60% done and omits what init files actually need first
   (`setq-default`, `kbd`, `identity`, `symbol-name`, …).
5. "Retain iterative implementations" is already prelude rule 2 (whose
   stated reason is itself stale — the bound is the 1097-frame arena, not
   the GC stack).
6. Wave C's `#'` bullet landed in 04D; its keyword bullet duplicates
   Wave A's.
7. Wave C's char-literal deferral rationale is dead (Phase 5 integers).
8. Wave D's add-list (`load`/`require`/`provide`/`featurep`/init
   discovery) all exist; the real gaps are diagnostics (`path:BYTEOFF`
   masquerading as a line; no runtime positions), dropped docstrings, and
   `declare`.

## Funding

Measured at the Phase 7 review close, 2026-08-06 (re-measure at slice
start — Rule 6):

- kg: scc **5714/5730** after the review fixes (the audit's mid-review
  4-point figure is superseded; `commandp` already landed in the fix
  cycle, so strike it from 08D's list if present). 08D/08E's C-side
  work (pin adaptations, `format` directives, loader diagnostics) is
  priced **+40..90 scc**, which does not fit 16 points of headroom:
  raise `SCC_COMPLEXITY_MAX` here by what the re-measured floor plus
  the band needs, proved live by temporary lowering, recorded with a
  dated Makefile comment.
- fe: scc **670/760**, `fe.c` **100/520**, `fe_eval.c` **453/520**,
  pmccabe **886/980 across 299 symbols**. 08B is priced **+25..45**
  (constant checks + keyword intern + hierarchy row); 08C **+60..110**
  (escape table, `?` decoder, radix, reject arms, line counting). Both
  fit the standing caps on these figures; if re-measurement disagrees,
  fund here with dated Makefile comments.
- Arena: `FeMinimumArenaSize()` **56880**, kg's 1 MiB arena **1097
  frames / 56223 slots**; the prelude batch costs live slots, not C
  bytes — record before/after `kg_lisp_arena_stats()` peaks in 08D
  (the audit's prototype measured +718 peak live).

## Slice-start remeasurement — 2026-08-06

The required Rule 6 remeasurement was run against the checkout at the start
of this slice, not copied from the Phase 7 close:

| tree/measure | result | command or tool |
|---|---:|---|
| kg scc total | **5714**, max file **479** (the standing file cap is 520) | `make complexity-check`, scc 3.7.0 |
| fe scc total | **670**, `fe_eval.c` **453**, `fe.c` **100** | `make -C fe complexity-check` |
| fe pmccabe | **886 / 299 symbols**, max function **15** | `make -C fe pmccabe-check` |
| Emacs oracle | **GNU Emacs 31.0.90**, build 1, 2026-07-09 | `/opt-3/emacs-31-lucid/bin/emacs -Q --batch` |

The kg scc cap is therefore funded at **5804**: the measured floor 5714 plus
the top of the stated +40..90 C-side band. The fe measurements remain inside
760/520/980, so neither fe cap moves in this documentation-only slice. The kg
temporary-lowering proof was run live and is repeatable without a fixture:

```
$ SCC_COMPLEXITY_MAX=5713 make complexity-check
FAIL: total complexity 5714 exceeds limit 5713
$ SCC_COMPLEXITY_MAX=5804 make complexity-check
scc total complexity: 5714 (limit 5804)
```

The raised value is also the checked-in default in `Makefile`; the exact proof
is repeated in its dated comment. This command-level proof follows the
project's existing complexity convention: `complexity-check` consumes the
live scc output, rather than a hand-maintained fixture that could drift from
the source tree.

## Corpus spelling audit — 2026-08-06

The init-facing Lisp corpus was searched before pinning. `lisp/prelude.el`
contains no executable `?`, `[`, radix `#`, or keyword literal spellings;
its only `#'` hit is in a comment describing function designators. The only
other source-tree bracket hit in `lisp/*.el` is a prose interval in
`lisp/auto-fill.el`. The new oracle cases intentionally contain every
spelling 08C must handle: `?` character and modifier literals, `#x`/`#o`/
`#b`, escaped strings, and `:foo` keyword values. No existing kg case was
rewritten or silently reinterpreted; the two diagnostic cases are explicitly
kg-policy and have no Emacs snapshot.

## What 08A does not do

No implementation, no pin move, no prelude edits. The `kgbatch` utility is
08D's to land (it needs a home under `test/` and a Makefile hook); 08A only
uses a scratch build of it to pin tables.
