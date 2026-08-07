# Sub-plan 12C — File conditions and the defvar scope carrier (fe)

Third of the twelfth set; requires 12B.  fe-only; closes the fe
workstream — **no pin**.  Three small measured pieces plus the
pre-pin cap re-set.

## Part 1 — `file-missing`

One hierarchy data line: `file-missing < file-error < error`
(`file-error` already exists at `fe_eval.c:123` and already works
from kg — the audit killed the stale TODO blocker claim).  Tests:
catch by `file-missing`, by `file-error`, by `error`; `signal` with
Emacs' data shape `("Cannot open load file" "No such file or
directory" PATH)` renders and compares as the oracle runner expects
(condition symbols exactly, data per the Phase 10 acceptance rules).
No kg raise-site changes here (12D's).  `permission-denied` is
recorded in the hierarchy row's rationale as measured-only-
unprivileged, not implemented.

## Part 2 — the one-arg-`defvar` scope carrier

Emacs' measured rule: a one-arg `(defvar v)` makes `let` dynamic in
the defvar's **own file** and stays lexical in a **later file**.  The
carrier is `EvaluateInput` (`fe/fe.c`), entered once per load — the
audit sized it at ~25–30 lines in `fe.c` (370 file-cap points free),
not `fe_eval.c`.  Design: the let-dynamic-only marking from a one-arg
defvar is scoped to the current input unit (recorded on entry,
unwound on exit — including abnormal exit through the barrier);
two-arg `defvar`/`defconst` marking stays global as Emacs' is.
`internal--mark-special`'s `full-p=nil` arm becomes the scoped one;
`special-variable-p` keeps answering `nil` for scoped marks (the
measured A7a pair stays true).

Enumerated tests: the two-file probe — file A `(defvar oav)` + a
dynamic `let` observed dynamic *within A*, then file B `let`s the
same name and is **lexical** (the leak the Phase 11 row could not
pin); the same across nested loads (A loads C; C's marks do not leak
back into A unless Emacs' measured answer says otherwise — measure
first, then pin); abnormal exit from a load mid-file unwinds the
scope; the existing single-file grid probes (A7a/A7b) stay green;
`special-variable-p` answers unchanged.  The trap from 12A: the
pinned `phase11-one-arg-defvar-file-scope` case does **not** flip
(it pins the oracle shim's per-form scoping, not the leak) — its
rationale is rewritten to say what it pins, and the new two-file
probe carries the fix's evidence.  (The kg-side compat case for the
two-file shape is 12D's, against real `load`.)

## Part 3 — close the fe workstream

fe manifest/doc edits for both parts (hand-edited — no XPASS on fe's
side); the nine-stage runner green; caps re-set at measured actuals
in the final commit, **pre-pin**, nothing landing after it; the
commit body carries the caps table and the per-part test census.

## Does not do

No kg edits, no pin, no `permission-denied` implementation, no
kg raise-site changes, no backquote, no pool work (kg-side, 12D).

## Gates

- All four fe gate commands green at every commit,
  exit-status-checked; `cd fe && .ci/run-ci-steps.sh` green at the
  slice head.

## Price

fe +6..14 scc / +8..16 pmccabe of the funded 835/1155 (one data
line; a scoped-marking carrier in `fe.c`).
