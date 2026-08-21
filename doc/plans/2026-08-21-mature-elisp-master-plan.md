# Mature Elisp support for fe + kg: master plan

Status: **proposed**.  Phase M0 is the entry gate.  Later phases are not
authorization to implement every item they name; each phase has an explicit
exit or selection gate.

This plan starts after Phase 29 U.1a and its external-review correctness
tranche.  It coordinates work that belongs in three repositories/layers:

1. `fe/tiny-regex-c`: the byte regexp engine;
2. `fe`: evaluator, reader, object model, conditions, and Fex boundaries;
3. `kg`: the Emacs-shaped prelude, editor primitives, package loader, and
   interactive acceptance surface.

It supersedes Phase 29's U.2/U.3 **hold as a scheduling document**, but does
not rewrite that plan's historical decisions or results.

## 1. Outcome and non-goals

The goal is a small, dependable Elisp implementation, not an undocumented
best effort at GNU Emacs.  “Mature” means all of the following:

* the implemented dialect is versioned and documented;
* accepted syntax and callable names either behave as documented or fail
  loudly at the unsupported seam;
* no standard feature is advertised when its observable contract is only a
  private subset;
* compatibility claims are backed by exact values, conditions, side effects,
  and representative package operations against the pinned Emacs oracle;
* an unmodified third-party library can be used inside a stated input domain;
* resource bounds, GC behavior, and editor responsiveness stay measured;
* unsupported areas are discoverable without reading implementation code.

The committed target is deliberately narrower than “runs ELPA”:

| maturity level | claim |
| --- | --- |
| **C0: contract-mature** | fe/kg's own Lisp surface has trustworthy tests, versions, and failure classifications |
| **C1: library-mature** | unmodified `s.el` is scenario-green for a published core input domain |
| **C2: ecosystem-mature** | one additional package vertical, selected for user value and bounded cost, is scenario-green |

Anything beyond C2 is a new decision.  In particular this plan does **not**
commit to full `cl-lib`, `compat`, byte compilation, EIEIO, package.el,
records, hash tables, Unicode normalization, or arbitrary ELPA packages.

## 2. The evidence at the entry point

The correctness-tranche worktree has:

* 499 Emacs-compared oracle cases: **434 agreements, 65 recorded
  divergences, 0 failures**;
* 428 kg manifest rows and 208 fe manifest rows;
* 59/59 kg native tests and 588 PTY passes / 6 tool-dependent skips;
* fe's fast suite, GC stress, payload suite, complexity, pmccabe, and format
  gates green;
* `make bench` green after replacing the unreachable arithmetic-loop GC
  assertion with an allocation-volume counter;
* unmodified `s.el` loading, with the existing smoke probe calling 73 of 74
  public functions without a raise.

Those are useful inputs, but “did not raise” is not value compatibility and
“loaded” is not package support.  The known `s.el` gaps remain concrete:

* contextual `$`/`^` regexp semantics make `s-lex-format` silently wrong;
* a condition declared through `error-conditions` is rejected by `signal`;
* non-ASCII case folding and grapheme/normalization behavior diverge.

The 110-package first-blocker census remains a prioritization instrument.  It
is not a success metric.  A package is never promoted by moving its first
failure or by reaching `(provide ...)` alone.

## 3. Rules that every phase carries

### 3.1 Compatibility labels

Use four different labels and never collapse them:

* **loads**: its top-level forms reached its `provide`;
* **smoke-green**: selected entry points completed without a raise;
* **scenario-green**: named operations produced the expected values,
  conditions, side effects, and saved/editor state;
* **supported**: every required scenario in a published input domain is
  green, all excluded domains are listed, and no dependency feature is a
  false capability claim.

Only the last two are product claims.

### 3.2 Standard features are capability promises

`(provide 'FEATURE)` for an Emacs standard library is allowed only when one
of these is true:

1. the supported contract for the selected Emacs baseline is complete; or
2. callers have a real, standard capability/version mechanism that selects
   only the implemented contract.

A prose note saying “named subset” does not satisfy either condition because
`require` and `featurep` cannot see the note.  Working names may remain
available unadvertised.

### 3.3 Measure before implementation

For each semantic change:

1. freeze Emacs' result first with a minimal oracle case and agreeing control;
2. classify the current kg answer as **silent-wrong**, **loud-unsupported**,
   **intentional-policy**, or **resource-bound**;
3. state the named package/editor consumer;
4. implement at the lowest owning layer;
5. flip the manifest status and re-run the same case.

Silent-wrong behavior outranks missing names.  A larger supported-name count
never compensates for introducing a plausible incorrect answer.

### 3.4 Layer and pin discipline

Work moves upward only:

* regexp grammar/matcher changes land and pass in tiny-regex-c first;
* evaluator/reader/Fex changes land and version in fe next;
* kg advances the submodule pin and adds only its adapter/prelude/editor half;
* each pin-moving commit names old/new versions and the exact oracle or
  scenario rows it changes.

No kg conditional should compensate for a semantic defect owned by fe or
tiny-regex-c.

### 3.5 Evidence and release discipline

* No test fixture fetches.  Sources used by CI are tracked with provenance.
* Oracle snapshots are runner-generated; hand edits are refused.
* A ratchet increase or decrease carries old/new measured proof and rationale
  in its commit message, as required by the repository notes.
* Counters are performance evidence before wall clock.
* GPL package source remains quarantined from shipped artifacts.
* Every phase ends with clean worktrees and reviewable commits; a dirty tree
  is not a landed release state.

## 4. Phase M0 — close the correctness tranche

Cost: **M** after the fuzz and parallel-matrix findings.  No new Elisp surface
may start before this exits.

### M0.1 Retract or complete `subr-x`

Current probe:

```elisp
(list (require 'subr-x)
      (featurep 'subr-x)
      (condition-case e
          (named-let loop ((x 1)) x)
        (error (car e))))
;; => (subr-x t void-function)
```

This is the same false-capability shape that caused `cl-lib` to be retracted.
`named-let`, `hash-table-keys`, `hash-table-values`, and
`hash-table-empty-p` are real consumers behind successful `subr-x` requires.

Decision for this phase: remove `(provide 'subr-x)` and keep the working
string/threading names unadvertised.  Completing the full selected-version
`subr-x` surface is a later, separately sized package vertical; do not grow
M0 to include hash tables just to retain the feature bit.

Freeze the retraction as an intentional divergence and update the API table.

### M0.2 Make the Fex NUL refusal cleanup-safe

The shared `FexCopyStringZ` refusal is correct for C-string boundaries, but a
later bad `execute` argument raises past the argv array and earlier copies:

```text
valgrind ./fe -e '(execute "true" "a\000b")'
29 bytes definitely lost (FexExecute)
```

Use a two-pass validation/copy design or a cleanup registered for the whole
argv vector.  The error path must release the vector and every earlier
argument before transferring control.  Add a later-argument death probe and
run it under the fe Valgrind lane.  Keep the existing exact-byte `read-file`
and `write-file` round trip.

Every C-string caller must be covered by either a direct test or a structural
caller census: open path, open mode, remove path, execute argv, regexp pattern,
and regexp subject.

### M0.3 Repair the version tripwires

The worktree documents “Fe 23.0” and sets `FE_LANGUAGE_VERSION` to 19, while
`FeVersion` and `test_api.c` still pin `22.0`.  Make one release truth:

* `FeVersion` and its runtime test move to `23.0` if this is the 23.0 cut;
* `FE_API_VERSION` remains 15;
* `FE_LANGUAGE_VERSION` remains 19;
* `FexVersion` remains mechanically tied to language version and gets a
  direct runtime assertion.

The release notes, headers, example host, kg static assertions, and benchmark
artifact header must agree.

### M0.4 Repair corpus identity and strengthen its gate

The three renamed U.1a fixtures and their snapshots currently retain their
old embedded `id`/`case` values.  Correct them, then extend the structural
checker so:

* `cases/NAME.json` requires `id == NAME`;
* `oracle/NAME.json` requires `case == NAME`;
* every Emacs-compared case has exactly one matching snapshot;
* the checker has a self-test proving both mismatches fail.

This is provenance, not decoration: diagnostics, snapshots, manifest rows,
and filenames must name the same experiment.

### M0.5 Record the real `eval` divergence

The current manifest says absent/nil `eval` behaves exactly as Emacs.  It does
not for a closure created inside the evaluated form:

```elisp
(condition-case e
    (funcall (eval '(let ((x 7)) (lambda () x)) nil))
  (error (car e)))
;; kg: 7             Emacs: void-variable
```

Fe evaluates lexically by default; Emacs' `LEXICAL=nil` evaluates
dynamically.  Add the oracle case, reclassify the row, and correct every claim
that nil is exact.  Do not implement a dynamic evaluator in M0.  Keep
`lexical-binding` at truthful `t`; the purpose here is to make the remaining
gap explicit.

### M0.6 Land ratchets with their evidence

Split or write commit messages that carry the required proof for:

* fe scc 1063 -> 1068 and pmccabe 1468/482 -> 1473/484;
* kg prelude reachable objects 10489 -> 10514, embedded bytes 108143 ->
  109566, definitions 156 -> 157, and payload peak 90272 -> 90840;
* the corresponding reductions, including peak-live 11899 -> 11865;
* kg's three currently unbanked pmccabe improvements:
  `editor_query_replace_regexp` 35 -> 34, `lisp_search_backward` 12 -> 11,
  and `lisp_search_row_forward` 10 -> 9.

Do not put historical rationale beside the knobs; the comments continue to
describe only the measured tree.

### M0.7 Bound cyclic writer work below the fuzz deadline

The full fe matrix found a two-node graph whose `car` and `cdr` cycles expand
until the default 8-million-node writer backstop.  It terminates, but the
ASan/UBSan fuzz build takes about 1.8 seconds and crosses the lane's 2-second
per-input timeout under load.  The exact input is:

```text
81 81 a4 a4 0f 52 86 81
```

Track the reproducer before changing the code.  Decide explicitly between a
general visited-object cycle detector and materially smaller public default
bounds; preserve `max_bytes`, `max_nodes`, `max_depth`, completion status,
allocation-callback safety, and existing printed-policy cases.  Add a counter
assertion for work performed so correctness does not depend on a wall-clock
threshold, then replay the seed under the fuzz and sanitizer lanes.

### M0.8 Make the parallel matrix load-safe

The six-lane kg run reproduced two infrastructure failures that all passed
when their lanes were rerun serially:

* `test_dap_session` exhausted its fixed polling budget in both the GCC
  analyzer and Valgrind lanes;
* the Emacs oracle lost startup input for `open-line-keeps-point` and the two
  query-replace-newline cases in the `WITH_LSP=0` lane, yielding baselines
  that did not represent the requested operations.

Make DAP test waits deadline/readiness based and scaled for parallel and
instrumented builds.  Increase or derive the Emacs-only startup cover for
parallel lanes so a slow oracle cannot turn lost input into an ordinary
saved-file mismatch.  Keep timeouts as ERROR/infrastructure diagnostics; do
not weaken expected editor results.  Re-run the full parallel matrix under
load, not just each lane in isolation.

### M0 exit gate

```text
fe:  make check complexity-check pmccabe-check format-check
kg:  make check complexity-check pmccabe-check format-check
kg:  make bench
kg:  make check-regex-differential
both repositories' full CI runners, including the expensive lanes
git status --short is empty in both repositories
```

The parallel kg runner must not be waved through because its failing lanes
pass serially; M0.8 must be demonstrated by the concurrent matrix.

## 5. Phase M1 — package-scenario evidence

Cost: **M**.  Owner: kg test infrastructure.  No evaluator change.

### M1.1 Add one small scenario runner

Reuse the existing canonical-record machinery instead of inventing another
comparison format.  A package scenario consists of:

* a tracked package source and provenance/SHA256;
* zero or more setup forms;
* one operation expression;
* a declared input-domain tag (`ascii`, `utf8-codepoint`, `editor`, etc.);
* Emacs' canonical value/condition record;
* optional saved-file or screen assertions for editor scenarios.

Suggested tree shape:

```text
test/elisp-packages/manifest.json
test/elisp-packages/s/*.json
test/elisp-packages/oracle/*.json
utils/check_elisp_packages.py
```

The runner may call the same Emacs shim and `test/kgbatch`; it must not add a
dependency.  Extend the PTY harness only for a scenario that truly requires
windows, input, or saved-file state.

### M1.2 Make support status machine-readable

For each package record:

* exact source/provenance;
* load result;
* required features and their advertisement status;
* scenarios by input domain;
* known exclusions and their manifest IDs;
* support label from section 3.1.

`make package-compat-check` regenerates nothing and runs in `make check`.
Snapshot generation remains an explicit oracle command.

### M1.3 Seed with controls, not only desired answers

The first `s.el` slice must contain:

* one load scenario;
* an exact-value operation from each selected family;
* one condition/error path;
* one macro-expansion path;
* UTF-8 and invalid-byte probes that expose, rather than hide, the domain
  boundary;
* one deliberately divergent control so the runner's XPASS rule is tested.

### M1 exit gate

The existing 73-call smoke remains useful but is renamed/reported as smoke.
No document calls it package support.  The new gate must fail on a wrong
value, wrong condition, stale source hash, missing scenario, stale snapshot,
and XPASS.

## 6. Phase M2 — make `s.el` core scenario-green

Cost: **M**, split by owning repository.  This is the committed C1 target.

### M2.0 Freeze the supported domain

Start with **ASCII plus byte-preserving strings**, which kg and fe can support
without pretending to have Unicode normalization.  Freeze representative
operations in these families:

* trim, whitespace collapse, split/join, lines, repeat, pad, truncate;
* prefix/suffix/contains, case-sensitive comparisons;
* literal and regexp replacement, match extraction;
* formatting and `s-lex-format`;
* error signaling and handler selection;
* empty strings, zero-width matches, embedded NUL where the operation is
  byte-safe, and invalid argument types.

Keep separate recorded divergences for non-ASCII case folding,
`multibyte-string-p`, combining marks, and canonical normalization.  C1 is
named **`s-core/ascii`**, not “all of s.el”.

### M2.1 Contextual `$` and `^` in tiny-regex-c

Implement Emacs' parsed-position rule already frozen by Phase 29:

* `$` anchors only at pattern end or before `\|` / `\)`;
* `^` anchors only at pattern start or after `\(` / `\(?:` / `\|`;
* elsewhere they are literal characters;
* escaped spellings remain literal;
* the existing subject anchors `` \` `` and `\'` remain unconditional.

Land tiny-regex tests first, including nested groups, alternatives,
quantifiers, bounded matching, and bad patterns.  Then advance fe and kg and
flip the differential/oracle cases.  `s-lex-format`'s exact expansion and
result are the package acceptance case.

### M2.2 Dynamic condition plists and `define-error`

Freeze and implement the two inseparable halves:

1. `signal` and handler matching consult a symbol's `error-conditions` plist;
2. `define-error` writes the message and condition ancestry Emacs exposes.

Preserve the static hierarchy as seeded properties rather than maintaining a
second hidden inheritance truth.  Freeze mutation after definition, multiple
parents, an unknown parent, middle-ancestor catches, cleanup/unwind delivery,
and `error-message-string`.  The decisive package scenario is s.el's own
`s-format-resolve` path.

### M2.3 Exact operation matrix

Replace “73 calls, none raised” as the main evidence with expected results for
representative public operations.  The smoke may still call all public names
to find new missing functions, but a family becomes green only through exact
scenarios.  Record any untested public family explicitly.

### M2 exit gate

* unmodified tracked `s.el` loads;
* every required `s-core/ascii` scenario agrees with Emacs;
* `s-lex-format` works with one and multiple bindings;
* the declared condition is catchable through its plist ancestry;
* Unicode/normalization exclusions remain loud in the support report;
* no standard dependency feature is falsely advertised;
* all ordinary, differential, sanitizer, fuzz, coverage, complexity, census,
  benchmark, and full-CI gates pass.

Stop here and publish C1 before selecting more package work.

## 7. Phase M3 — classify and burn down silent divergences

Cost: **M**, performed in small independent waves.

Add a required `failure_mode` to every non-supported manifest row:

* `silent-wrong`;
* `loud-unsupported`;
* `intentional-policy`;
* `resource-bound`.

Then work in this order:

1. silent-wrong cases exercised by C1 scenarios;
2. silent-wrong reader/condition/regexp cases with named package demand;
3. loud gaps required by the selected C2 package;
4. everything else only when a consumer is named.

The Phase 29 reader item belongs here: unknown string escapes become the
escaped character, except the separately frozen space/newline continuation
and known escapes.  This is a fe language-version change and must preserve
writer/read-back guarantees.  It is a likely prerequisite for `dash`, but not
for C1 and therefore does not ride M2.

Word/symbol regexp boundaries and explicitly numbered groups also stay here
until selected.  They need a design before code: Emacs boundaries depend on
syntax classification, while tiny-regex-c currently has only a fixed byte
classifier.  Do not implement `\<`, `\>`, `\_<`, or `\_>` as aliases that are
right only in one mode.  Decide whether the engine API takes a classifier
callback, whether Fex exposes a fixed standalone policy, and how kg supplies
the current buffer's syntax semantics.  Until that design lands,
`regexp-opt` continues refusing the affected PAREN values loudly.

## 8. Phase M4 — select exactly one C2 package vertical

Cost: **S for selection**, implementation cost decided afterwards.

Re-run the package census after C1, but score candidates on:

| factor | question |
| --- | --- |
| user value | does this enable a real kg workflow rather than merely load? |
| scenario quality | can representative behavior be tested without fetching or external services? |
| dependency closure | how many standard features and packages must be honestly completed? |
| semantic leverage | does the work improve fe/kg generally or add package-specific façade? |
| failure risk | does a shortcut risk silent misbinding or wrong editor state? |
| ongoing cost | can the supported source/version stay pinned and auditable? |

Candidates may include `dash` (large dependency multiplier but a longer
reader/macro/editor chain), completing `subr-x`, or a small editor utility.
`inheritenv` is not selected merely because it once loaded: its useful path
needs `cl-letf*`, generalized variables, process/environment variables, and
advice semantics.  It must win on user value, not on package count.

The selection deliverable is a short addendum containing:

* exact package/version and source provenance;
* three to ten representative scenarios;
* transitive feature closure;
* missing semantic families and estimated owner/cost;
* a stop budget;
* an explicit owner decision to proceed or defer.

No implementation starts in M4.

## 9. Conditional substrate projects after selection

These are dependency branches, not a queue.  M4 opens only what its chosen
vertical requires.

### 9.1 `eval` and lexical environments

Freeze three separate contracts before designing storage:

1. absent/nil `LEXICAL`: dynamic evaluation with no caller lexical bindings;
2. `t`: lexical evaluation with an empty lexical environment;
3. an alist/environment object: lexical evaluation with supplied bindings.

Fe's ordinary evaluator is already lexical, so accepting `t` may be a small
honest slice; it must still be proved across escaping closures, specials,
conditions, throws, cleanups, and budgets.  Nil is a different semantic mode,
not an alias for the same path.  An alist and `macroexpand` environment may
require an environment representation/ADR.

The same project owns the existing one-argument-`defvar` carrier residual:
a definition after the declaration must retain the declaration's dynamic
binding rule when called from another input unit.  Do not solve one by adding
a second incompatible environment model.

Before implementation, record lookup/allocation/frame counters and the
current env-width/env-depth properties Phase 21 intentionally made into
tripwires.

### 9.2 Generalized variables and `cl-lib`

Sequence, if selected:

1. freeze a small generalized-place protocol (`symbol`, `car`, `cdr`,
   `nth`, `aref`, `default-value`, and package-demanded setters);
2. implement one `gv` expansion seam;
3. build `cl-letf`/`cl-letf*` and upgrade `cl-incf` on that seam;
4. run exact package scenarios;
5. only then reconsider `provide 'cl-lib`.

`cl-defun` is a separate project and must implement real `&key` semantics,
defaults, supplied-p variables, duplicate/unknown keywords, and
`&allow-other-keys`.  A `defun` alias remains forbidden.  `cl-loop`, records,
`cl-defstruct`, and `compat` remain deferred unless selected independently.

### 9.3 Editor-definition surface

Names such as `defgroup`, `define-minor-mode`, keymap constructors, hooks,
advice, font-lock, and process variables belong in kg.  A no-op macro is
acceptable only when every selected scenario proves its metadata and side
effects are irrelevant.  Otherwise implement the observable editor state and
test it through PTY/saved-file outcomes.

Do not reintroduce `emacs-major-version` to steer packages around missing
surface.  The version variable returns only after the project can state what
Emacs-version capability contract it represents.

## 10. C2 exit and maintenance

C2 exits when:

* C0 and C1 remain green;
* the selected package loads unmodified from its tracked source;
* its representative pure and interactive scenarios are exact;
* its dependency features are honestly advertised or not advertised;
* every remaining divergence in its declared domain is explicit and loud;
* fe/kg/tiny-regex versions and submodule pins agree;
* full CI, expensive lanes, regex differential, fuzz seed replay, coverage,
  complexity, startup census, and benchmark gates pass;
* README and `doc/lisp-api.md` list supported packages **with their input
  domains**, not a raw package count.

After C2, update the pinned package corpus deliberately, never automatically.
A source update is treated like a dependency update: diff it, rerun scenarios,
and change the support record only with evidence.  Re-run the broad census to
choose work, but keep the product dashboard centered on scenario-green
packages and failure-mode debt.

## 11. Stop conditions

Stop and ask for an owner decision when any of these is true:

* the selected package requires broad `cl-lib`, `compat`, bytecode, or EIEIO;
* a standard feature cannot be completed within the declared phase budget;
* correctness requires a new Unicode database or external runtime dependency;
* the implementation needs a second semantics hidden behind the same name;
* a ratchet must rise without a local, measured explanation;
* the only reported gain is more files reaching a later blocker;
* C1 regresses while pursuing C2.

The acceptable outcome of an M4 selection is **defer**.  A small editor with a
truthful, stable C1 library profile is more mature than one which claims a
large package surface and fails after `require`.
