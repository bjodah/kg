# Sub-plan 12A — Pin the remainders, decide the shapes, fund the phase

Second post-program set; first of the twelfth; no prerequisites.  Like
every A-slice it changes no behaviour.  Phase 12 is the recorded
remainders Phase 11 left with reproductions: the cleanup-handler gap,
throw-across-load, the file condition classes, the excursion pool
bound, and the one-arg-`defvar` scope approximation — plus the one
item measured **out** (backquote).  Every number below was measured
2026-08-07 at kg `4e2dc81` / fe `82347b3` by the Phase 12 fact audit
(`p12-fact-audit.md` in the session scratchpad carries the full
reproductions; implementers read it); per Rule 6 the implementing
slices re-measure at slice start.

## The measured findings that shape the set

1. **The cleanup gap is broader and simpler than Phase 11 recorded.**
   Not just "a cleanup that handles its own error clobbers the
   in-flight condition": a `condition-case`/`ignore-errors` inside an
   `unwind-protect` *cleanup* never handles anything at all, drain or
   no drain — `(unwind-protect 'body (ignore-errors (car 6)))` is
   `body` in Emacs and escapes to the host in kg.  `catch`/`throw`
   inside a cleanup already works.  The seam is one ordering:
   `fe/fe_eval.c:592` tests `cleanup_catch` before `:607` calls
   `FindConditionHandler`.  06A Decision 4 is measured **correct** and
   must be preserved: an *unhandled* cleanup error during an
   error/quit/throw unwind replaces the completion, byte-identically
   to Emacs, in all three measured probes.  Hazard: native cleanups do
   not republish `run_base`; the fix needs a frame floor taken from
   the value `RunOneCleanupEntry` already saves at `:346`.
2. **Shape B's decisive measurement: Emacs' `load` is incremental.**
   A file whose form 1 sets a global and whose form 2 is a reader
   error runs form 1 in *both* Emacs and kg — so an eager
   read-all-forms design would break fidelity in the opposite
   direction, and would also lose kg's per-form `path:LINE` labels
   (latched per form at `fe/fe.c:2244`; Emacs has no file:line — the
   label is a kg extra worth keeping).  The viable shape is **(c)**:
   fe gains an `eval` primitive — six touch points, an arm modelled on
   the existing `FeFrameRelay` redispatch, no new frame kind — and
   kg's `load` becomes prelude Lisp looping an *incremental* reader
   native over the file, `eval`ing each form inside the current run,
   so a `throw` from a loaded file reaches an enclosing `catch` and
   error timing stays Emacs'.  fe has no `eval` today; kg has no
   `eval`/`read`/`intern`/`symbol-name` — this set adds only `eval`.
3. **`file-error` already exists in fe and works from kg**
   (`fe_eval.c:123`) — the `doc/TODO.md` claim that a hierarchy
   blocker exists is stale.  `file-missing < file-error` is one data
   line.  Exactly two kg raise sites matter (the loader's
   cannot-open and `require`'s cannot-find).  Emacs also raises
   `permission-denied`; it is measurable only unprivileged (root
   defeats `chmod 000`), so it is recorded, not asserted, where the
   box cannot measure it.  Three existing kg assertions break and are
   updated with the flip; no compat XPASS trap exists for this item.
4. **Backquote is blocked, not merely expensive.**  After renaming the
   reader's three expansion strings, the prelude could not even
   *spell* the macro: `` ` `` and `,` are reader delimiters and `\`
   escape syntax is a named reader error pinned by a **supported**
   policy row and two C tests.  Context-sensitive comma printing
   additionally needs a Writer state field that does not exist.  83
   sites in 24 files, guaranteed XPASS churn.  **Out**, recorded with
   this measured basis (Decision 6).
5. **The pool answer is 256, and back-pressure is measured useless.**
   At the nesting threshold every record is live — a forced full
   collection frees zero — so no collect-now entry is built.  256
   records cost +16.5 KB `.bss` and zero scc; nesting then runs to
   218 and hits fe's frame limit, i.e. the pool stops being the
   binding constraint.  One existing assertion breaks (updated with
   the change).  A real defect rides along: `Makefile:546` omits
   `lisp_obj.h` from a dependency list, so changing the pool size
   silently corrupts state on incremental builds — fixed first.
6. **The one-arg-`defvar` "impossible" is wrong.**  Emacs' real rule,
   measured for the first time: the marking is dynamic in the
   defvar's own file and **lexical in a later file**; kg's global
   flag leaks across files.  The carrier the Phase 11 row said does
   not exist is `EvaluateInput` (`fe/fe.c`), entered once per load —
   ~25–30 lines, in `fe.c` not `fe_eval.c`.  Trap: a correct fix does
   **not** flip the pinned `phase11-one-arg-defvar-file-scope` case
   (the oracle shim evaluates each setup form in its own scope — the
   case pins the shim's asymmetry, not the leak); the fix needs a new
   two-file probe case, and the old row's rationale is rewritten to
   say what it actually pinned.

## Decisions

1. **Scope: the five items above; backquote is out with its measured
   blocker recorded.**  No `read`/`intern`/`symbol-name`, no
   `NOERROR`/`NOMESSAGE` `load` arguments beyond what kg has, no
   buffer-locals, no `permission-denied` assertion where
   unmeasurable.
2. **The cleanup fix changes handler *visibility inside cleanups*
   only.**  The ordering at `fe_eval.c:592/:607` starts honoring
   handlers established inside the running cleanup (with the frame
   floor from `RunOneCleanupEntry`'s saved value), while an unhandled
   cleanup error keeps 06A Decision 4's replace-the-completion
   behaviour byte-for-byte — the three measured agreement probes
   (A10/A15/A16 in the audit) become permanent regression guards.
   With the gap fixed, the Phase 11 carried-forward divergence gets
   its manifest row as `supported` with fresh cases on both sides.
3. **`eval` is an fe core primitive with Emacs' one-argument shape**
   (the LEXICAL argument is rejected by name like macroexpand's
   ENVIRONMENT), evaluating in the current run so conditions, throws
   and quits propagate to enclosing handlers; `load` moves to the
   prelude as a loop over a new kg native `internal--read-form`
   (incremental: reads and returns the next form with its `path:LINE`
   label latched, nil-vs-eof distinguished), keeping C ownership of
   path resolution, depth bookkeeping and buffer lifetime.
   Throw-across-load's `divergent` row flips; nested-load and
   cleanup-during-throw cases ride the flip.
4. **`file-missing` lands as one fe hierarchy line; kg's two sites
   raise it with Emacs' data shape.**  The message-level compare
   stays byte-oriented where the oracle cases already compare
   condition symbols exactly (the Phase 10 acceptance rework).
5. **The pool goes to 256 with the Makefile dependency fix landing
   first**, and the pool-bound row's rationale is rewritten to name
   the frame limit as the new binding constraint (with the measured
   218).
6. **Backquote:** the `phase8-reader-backquote-symbol-names` row
   gains the measured blocker (reader delimiters + escape-syntax
   policy row + Writer state gap) so the next reader-phase can price
   it honestly; `doc/TODO.md`'s item is rewritten to the same basis.
7. **Funding.**  Bases at `4e2dc81`/`82347b3`: kg scc **5806/5806**,
   fe scc **806/806**, fe pmccabe **1121/1121** (359 symbols),
   `fe_eval.c` **509/520**, `fe_run.c` 25.  Raises: **fe 806→835 scc
   and 1121→1155 pmccabe in 12B's opening commit** (the eval arm, the
   ordering fix, the hierarchy line, the scope carrier); **kg
   5806→5830 scc in 12D's opening commit** (the incremental reader
   native and the two condition sites; the prelude `load` deletes C).
   Temporary-lowering proof each; 12C re-sets fe's caps pre-pin; 12E
   re-sets kg's at close.  Four items land in `fe_eval.c` (11 points
   of file headroom): if the file cap threatens, the next seam is
   already named in fe's Makefile (`fe_eval.c:230-700`) — a split is
   funded in preference to a file-cap raise.
8. **Versions:** `FE_LANGUAGE_VERSION` 9→10 (new name `eval`, changed
   cleanup-handler and load semantics), `FE_API_VERSION` stays 7
   unless a public C entry is added (none is planned — decided
   in-slice if falsified), `FeVersion` "11.0".
9. **Counting discipline:** the oracle runner's "11" is 11 divergent
   *cases*; the manifest carries **17** divergent *features*.  Every
   Phase 12 count names its unit — the Phase 11 close conflated them
   once.

## Work

1. This document, the README's Phase 12 sections, the price rows.
2. Raises land in 12B/12D opening commits.
3. Re-measure the caps/baseline table and record it in the README
   baseline paragraph (same commit as this document).

## Gates

- No behaviour change; `make check` identical before/after.
- README Phase 12 rows and this document agree on every figure.

## Price

0 scc both trees (documentation only).
