# 08D — The pin, and the library that costs nothing

Parent: [Phase 8](../2026-08-03-elisp-subset-and-fe-evaluator.md#12-phase-8--core-init-file-compatibility-roadmap).
kg-side. Two parts in one slice because the second is only testable after
the first: the phase's single pin move (08B+08C land together), then the
prelude batch the audit already prototyped at **zero measured scc**.

**Prerequisite:** [08B](08b-constants-and-keywords-in-fe.md) and
[08C](08c-an-honest-reader.md) merged on fe `analyzers-etc` with fe's full
CI green.

## Part 1 — the pin

One atomic commit: gitlink to the post-08C fe SHA, `doc/fe-upstream.md`
row (versions, `FeMinimumArenaSize()`, 1 MiB frames/slots — re-measured at
the pin, never carried forward), `static_assert` tripwires to the new
`FE_LANGUAGE_VERSION`, and the kg-side adaptations the fe changes force.
Budget the adaptations honestly before betting on "none": the reject arms
(08C.1) are the risk — any kg test or prelude line that *relied* on a
misreading (a `?` or `[` reaching Lisp as a symbol) breaks at the pin.
Audit the corpus for those spellings before the fe slices land, in 08A's
verification pass, so this pin is measured rather than hoped. Keyword
self-evaluation can also break any kg test that used `:name` as an
ordinary assignable symbol.

## Part 2 — the prelude batch (kg-only, after the pin)

All measured against 08A Table L; every item was prototyped by the audit
against the oracle. In `lisp/prelude.el`, iterative (rule 2), with
`PRELUDE_DEFS` bumped once:

1. **Wave B remainder**: `assq`, `mapc` (returns its list argument),
   `mapconcat`, `nreverse` (`setcdr`-based, destructive), `delq`,
   `delete`, `add-to-list` (with the measured membership/APPEND
   contract), plus `identity`, `prog2`, `max`, `min`.
2. **Two prelude bug fixes**: `cond` answers a bodyless clause's test
   value (`(cond (5))` → 5); backquote handles dotted unquote
   (`` `(1 . ,x) ``) and nested-depth correctly (both audit-measured
   divergences today, both prelude macros).
3. **`defvar`/`defconst` docstrings captured, not dropped**; and the
   `(defvar x)` no-value form per 08A's decision (implement Emacs'
   declare-without-binding only if a cheap prelude/native spelling
   exists; otherwise bind-to-nil stays and the divergence is recorded
   with the measured Emacs answer).
4. **`documentation`** over a prelude alist (`internal--doc-put` at
   `defun` time), returning the docstring or nil — the audit proved this
   needs no property lists and no fe change, which retires
   `doc/lisp-api.md`'s recorded excuse.
5. **`setq-default` / `setq-local`** as documented aliases of `setq`
   (08A Decision 4) with the divergence recorded.
6. **`kbd`** returning the string kg's `keybind.c` grammar accepts —
   parse-validating "C-c …" spellings so `(global-set-key (kbd "C-c k") …)`
   works and anything outside the bindable set errors clearly.
7. ~~`commandp`~~ — landed in the Phase 7 review-fix cycle (`3c81f07`,
   a recorded-divergent native); nothing to do here beyond not
   duplicating it.

The audit's `kgbatch` harness lands here as a tracked test utility
(`test/kgbatch.c` + a Makefile convenience target, excluded from the
suite's pass/fail — it is a driver, not a test), so the init-file
experiment becomes repeatable.

**The proof:** 08A's 30-line init file, planted via `config_files:`, loads
end-to-end with zero errors in a PTY case — the phase's headline
acceptance. (Line 31 `custom-set-variables` and any other form scoped to
08E get their final spellings there; keep the 08D init-file variant to
what 08D ships, and extend it in 08E.)

## Tests owned by this slice

- Pin part: whatever the reject-arm audit forces, plus tripwire updates.
- `test/test_lisp.c`: per-function contracts for every new definition
  (Table L rows), the two macro fixes, `documentation`, alias behavior,
  `kbd` accept/reject, arena-stats before/after the batch (peak live
  recorded in the commit body; the audit measured +718 slots — re-measure).
- PTY: the init-file case; a `kbd`-bound command case.
- Manifests: new supported rows with runnable cases; retire
  `reader-char-literal-unsupported` (08C landed it); census note counts
  re-measured.

## Documentation

`README.md` + `doc/lisp-api.md`: the library list grows; `setq-default`
and `load-path` divergences documented; `doc/kg.1` only if user-visible
keys change (none expected). `doc/TODO.md`: strike what this lands.

## Gates

kg's Rule 9: both complexity commands at start and end (expect **≈+0 scc**
for Part 2 — that claim is this slice's own headline, verify it), `make
check` idle, `WITH_LISP=0` matrix, header/docs/lisp-compat/lisp-prelude/
lisp-include/gateway checks, coverage against floors, and the full
parallel runner at the phase close (08E) rather than here.

## What this does not do

- No `custom-set-variables`/`defcustom` (08E), no `format` work (08E), no
  new C natives beyond a checked `commandp` gap, no `load-path`-as-Lisp-
  variable, no `string-match`/regexp Lisp surface (needs its own measured
  slice — record as a Phase 8 second-set candidate), no `symbol-name`/
  `intern` (same), no property lists, no vectors.
