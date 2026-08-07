# Sub-plan 11D — The pin, the prelude switch, and the loader seam (kg)

Fourth of the eleventh set; requires 11C.  kg-side: the phase's single
pin move, then every behaviour adoption, with each divergence flip and
its manifest edit in the same commit (the XPASS rule, 11A Decision 7).

Opening commit: the funded kg raise, scc 5802→**5825** with
temporary-lowering proof (11A Decision 8).

## Part 1 — the pin

One commit: gitlink to 11C's fe head, the
`FE_LANGUAGE_VERSION`/`FE_API_VERSION` reconciliation in
`src/lisp_core.c` (9/7), the Makefile adaptation for the new fe TU
(kg compiles submodule TUs by name — `src/fe.o src/fe_eval.o` gains
the 11B split's sibling, with the same header-prerequisite discipline
`4414999` added), and `doc/fe-upstream.md`'s pin/version rows.  Green
`make check` before any behaviour adoption — divergent rows still
diverge at this commit because kg's prelude has not switched yet
(`defvar` does not mark, `let` is still a lambda application), which
is itself worth asserting: the pin alone must not flip any oracle
case.

## Part 2 — dynamic binding adopted (one commit, prelude + manifest)

- `lisp/prelude.el`: `defvar` calls `internal--mark-special` (full
  for two-arg, let-dynamic-only for one-arg; `defcustom` inherits via
  its `defvar` expansion), `defconst` marks full; the `let` macro is
  **deleted** in favour of fe's core bindings-list `let`; `let*` stays
  on `internal--let` (now special-aware).  Uninitialized-binding and
  degenerate shapes (`(let (a b) …)`, `(let () …)`) must keep their
  current meaning — verify against fe's core form before deleting the
  macro, and keep the suite green as the proof.
- New kg compat cases, `comparison: emacs`, spelling the 11A grid with
  real `defvar`/`defconst` (A1, A2a, A3a, A3b, A5, A6a, A7a, A7b,
  A7d, A8a, A8b, A9a, A10a, A11) plus the guards as agree-cases (A4,
  A6b, A13).  New case names, fresh runner-produced snapshots only —
  never regenerate an existing snapshot.
- Same commit, the XPASS edits: `prelude-defvar` → `supported`
  (rationale: the marking model, the one-arg scope approximation, the
  Emacs-identical capturable-temporaries exposure — 11A Decisions
  2–3); `prelude-defconst` rationale gains the special-marking fact
  (it is `null` today); `defcustom`'s "no dynamic binding" sentence
  goes; `phase8-library-contracts` re-checked against A7a/A7b (the
  pinned `(nil x nil)` answer is unchanged — assert that).
- `test/test_lisp.c`: the grid through kg's entry points, plus the
  three-PTY-case check that `lisp/auto-fill.el`'s free reads of
  `fill-column`/`auto-fill--error` still behave (they are only ever
  `setq`'d today — the audit found zero let-shadowing sites in the
  tree beyond the probe itself, so **no behaviour in any planted init
  may change**: assert the full PTY suite green as the evidence, and
  say so in the commit body).

## Part 3 — the loader seam (one commit)

`lisp_eval_file()` (`src/lisp_io.c:745`) moves to
`FeTryEvaluateStringWithOptions`: on a contained non-normal
completion, unwind the load bookkeeping this frame owns
(`load_depth`, `load_buffers`, the malloc'd buffer — the lines that
are unreachable today) and `FeResignal`, so an enclosing
`condition-case` catches (the `load-error-condition-reachability`
flip, manifest edit in this commit).  `native_load` returns `t` on
success (Emacs' answer; new oracle case).  Keep: reader-failure
catchability (already true), `require`'s cleanup-registry pop from
`4414999` (re-run its two-retry regression), the depth-limit and
missing-file errors' catchability.  New recorded divergences, added
not fixed: throw-across-load (case pinning kg's `no-catch` against
Emacs' `99` — Shape B rejected by scope, 11A Decision 5) and
`file-missing`-vs-`error` class for missing files (record in the
existing load rows' rationales; no new condition subtype).

## Part 4 — catch/throw across the two prelude forms (one commit)

Two capture/restore natives in `src/lisp_buffer.c` (point/mark/buffer
state for `save-excursion`; current-buffer switch for
`with-current-buffer`), and the two forms become prelude macros over
Lisp `unwind-protect` (the fix `catch-throw-reachability`'s own row
names).  Flip that row in the same commit.  Guards: `condition-case`
still crosses both (existing case stays green); restore runs on
error, throw, and quit paths; the hook/process-filter/
`command-execute` walls stay walls (their rows re-worded, not
flipped); `WITH_LISP=0` untouched.  The excursion state the natives
capture must match what the C forms preserve today — read
`src/lisp_buffer.c:562/:596`'s current implementation and preserve
its exact scope (no more, no less), pinning it with a PTY case that
edits across the boundary.

## Part 5 — the writer flip, kg side (one commit)

kg inherits the abbreviation at the pin, so this commit is tests and
manifest only: `test/test_lisp.c:3102-3103` (`%S` of quote forms)
updated to Emacs' answers, `writer-quote-abbreviation` → `supported`,
`phase8-reader-nested-backquote`'s rationale updated (quote half
closed, backquote stays), echo-area PTY case for `M-:` showing `'x`.
Nothing else in the tree prints a quote form (audit: 2 kg sites
total) — a third site appearing means stop and re-audit.

## Does not do

No docs sweep beyond the manifest and fe-upstream rows named above
(11E owns README/kg.1/lisp-api/TODO/ChangeLog), no close mechanics,
no cap re-set (11E), no buffer-locals, no Shape B, no backquote.

## Gates

- `make check` green at every commit (the XPASS rule makes same-commit
  manifest edits load-bearing); `make lisp-oracle-check` count moves
  13 → 9 recorded divergences by Part 5 (minus defvar, quote, load,
  throw; plus the new throw-across-load row — record the exact final
  count, expected 10 total with the addition).
- `make WITH_LISP=0 clean all check` green after Parts 2 and 4.
- Suite counts recorded per commit body; arena figures re-measured at
  the pin (Phase 10 close: 57680 min / 56224 slots / 487 live / 5210
  after prelude — the prelude switch may move the last figure; record
  it).

## Price

kg scc +8..20 of the funded 5825 (the natives and the loader seam;
prelude/cases/PTY are outside the scan).
