# Sub-plan 12D — The pin, the Lisp loader, and the pool (kg)

Fourth of the twelfth set; requires 12C.  kg-side: the phase's single
pin move, then the behaviour adoptions, each flip sharing its commit
with its manifest edit (the XPASS rule).  Opening commit: the funded
raise, kg scc 5806→5830, temporary-lowering proof.

## Part 0 — the Makefile dependency fix, before anything

`Makefile:546` omits `lisp_obj.h` from a dependency list, so a pool
resize silently corrupts state on incremental builds (the audit's
found defect).  Fix it as its own first commit with the
demonstration in the body, *before* the pin — it is a pure kg build
fix with no fe dependency and the pool work depends on it.

## Part 1 — the pin

Gitlink to 12C's fe head; `FE_LANGUAGE_VERSION` 10 reconciliation;
`doc/fe-upstream.md` pin/version rows.  Assert what the pin alone
changes: the cleanup-handler fix and `eval` arrive with it — run the
oracle suite at the pin commit and record which cases flip; any flip
rides this commit with its manifest edit and the reason stated (the
Phase 11 precedent).  The Phase 11 carried-forward cleanup divergence
had **no row** — it gains its `supported` row + kg case here (fresh
snapshots only).

## Part 2 — `load` becomes prelude Lisp over an incremental reader

New kg native `internal--read-form` (12A Decision 3): reads and
returns the next form from the named load stream with kg's per-form
`path:LINE` label latched (the fe reader latches at `fe/fe.c:2244`),
distinguishing end-of-file from `nil`; C keeps path resolution,
depth bookkeeping, buffer lifetime.  `load` (and `require`'s loading
arm if it shares the seam — read `src/lisp_require.c` first) becomes
a prelude loop `eval`ing each form in the current run.

Enumerated tests: **error timing preserved** — form 1 runs before
form 2's reader error surfaces (the audit's decisive Emacs
measurement, pinned as a case); `path:LINE` labels on errors from
loaded files unchanged (existing diagnostics cases stay green);
**throw-across-load flips** — `(catch 'outer (load thrower))` → 99,
with the `divergent` row flipped in the same commit; a catch inside
the loaded file catching its own throw; nested loads with the inner
throwing; unwind-protect cleanups in the loading frame running when
a throw crosses; `condition-case` around `load` still catches
(Phase 11's flip stays green); `load` still answers `t`;
`file-missing` from the two kg raise sites (cannot-open,
`require`-cannot-find) with Emacs' data shape — catch by
`file-missing`/`file-error`/`error` pinned as oracle cases, the
three existing kg assertions the audit named updated with the flip;
depth-limit behaviour preserved; quit during load still quit;
`WITH_LISP=0` green.  The two-file defvar-scope probe (12C Part 2's
kg half) lands here against real `load`, with the old row's
rationale rewritten per the 12A trap.

## Part 3 — the pool

`LISP_OBJ_POOL` (or its spelling in `src/lisp_obj.h:64`) 64→**256**
(+16.5 KB `.bss`, zero scc); the one breaking assertion updated;
nesting regression re-measured (expect the frame limit near 218 to
be the new binding constraint — pin the measured value, not the
audit's); the sequential-5000 and cumulative regressions stay green;
the pool-bound manifest row's rationale rewritten to name the frame
limit.  No collect-now entry, with the audit's measured uselessness
(all records live at threshold) recorded in the rationale.

## Does not do

No docs sweep beyond named manifest/fe-upstream rows (12E), no close
mechanics, no `read`-exposed-to-Lisp beyond the internal native, no
NOERROR/NOMESSAGE arguments, no `permission-denied`, no backquote.

## Gates

- Every commit green on `make check`, `make complexity-check`,
  `make pmccabe-check` and the format gate, exit-status-checked;
  coverage gate at src-touching commits (the Phase 11 A0 lesson —
  now a standing rule).
- Oracle counts recorded per commit body **with units** (cases vs
  features — 12A Decision 9).
- `make WITH_LISP=0 clean all check` green after Parts 2 and 3.

## Price

kg +10..24 of the funded 5830 (the reader native and condition
sites; the prelude loop deletes C — record the net honestly).
