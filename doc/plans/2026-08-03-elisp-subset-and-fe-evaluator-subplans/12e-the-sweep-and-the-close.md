# Sub-plan 12E — The sweep and the close (kg)

Fifth and last of the twelfth set; requires 12D.  No behaviour
change: every document that described the five items' old behaviour
tells the new truth, the backquote blocker is recorded where the
next phase will look, and the phase is re-measured and handed to
review.

## Part 1 — the sweep (by content, not literal grep — the Phase 11
lesson)

- **`doc/TODO.md`** — the cleanup-clobber item closes; the
  throw-across-load item closes; the stale `file-error`-blocker
  claim (`:423` at audit time) is corrected and the file-condition
  item closes; the backquote item is rewritten to the measured
  blocker (reader delimiters, escape-syntax policy row, Writer state
  gap — 12A Decision 6); the one-arg-defvar item closes with the
  scope rule stated; new items only for what Phase 12 newly recorded
  (`permission-denied` unmeasured-as-root; `eval`'s LEXICAL argument;
  the frame limit as the nesting bound).
- **`doc/lisp-api.md`** (version bump per its own rule) — `eval`
  documented with its propagation semantics; the loader section's
  catchability prose extended with `file-missing` and
  throw-across-load; the unwind-protect section's cleanup-handler
  rule; the excursion nesting bound updated.
- **`README.md` + `doc/kg.1`** (+ `make docs-check`) — the
  "Where it differs" list updated (items closed; the backquote and
  remaining walls re-stated precisely); `load`/`require` sections
  gain the condition classes; the roff twins match.
- **`doc/fe-upstream.md`** — retire/rewrite the rows the phase moved
  (cleanup visibility, throw-across-load, file classes, defvar
  scope); version rows verified against the 12D pin.
- **`test/lisp-compat/README.md`** — counts re-stated **with units**
  (cases vs divergent features — the 12A Decision 9 discipline).
- **`doc/ChangeLog.md`** — the phase's entry.
- fe docs: verify 12B/12C left them true; anything stale is filed
  back (a post-pin fe commit forces a second pin move — not this
  slice's call).

## Part 2 — the close

- Re-measure and record: both trees' caps re-set at measured actuals
  (fe's verified unchanged from 12C's pre-pin re-set), suite counts,
  oracle counts with units, compat census, arena figures,
  `WITH_LISP=0`, coverage.
- No Status section, no document retirement — the reviewer's acts.
  The final commit body carries the close figures the Status will
  cite.
- Final green light: `JOBS=8 .ci/run-ci-steps.sh --parallel` 12/12,
  recorded in the close commit body.

## Does not do

No behaviour change, no fe edits, no Status, no doc retirement, no
cap raises.

## Gates

- `make check`, `make docs-check`, `make lisp-compat-check` green;
  the stale-claim content sweep finds zero live sites; 12/12 at the
  slice head.

## Price

0 scc both trees.
