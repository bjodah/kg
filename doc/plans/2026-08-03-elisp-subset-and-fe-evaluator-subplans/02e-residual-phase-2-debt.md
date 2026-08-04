# 02E — Close Phase 2's carried-forward debt

Parent: [Phase 2](../2026-08-03-elisp-subset-and-fe-evaluator.md#6-phase-2--hard-cut-assignment-and-numeric-),
kg-only.  No fe change, no pin move, no language change.

**Prerequisite:** none beyond Phase 2 being landed.  This slice touches
nothing Phase 3 touches, so it runs **in parallel with
[03A](03a-measure-and-fund-the-frame-machine.md)** rather than blocking it.

## Outcome

The three items the set README lists under "Carried forward" are closed or
converted from a gap into a recorded, reasoned decision.  Nothing here is
new capability; it is the debt two sub-plan sets deferred, and it is small
enough that deferring it a third time costs more in re-derivation than
doing it.

Two of the three came into Phase 2 from the first set and were not closed
by it.  Neither blocked anything, which is exactly why they are still open;
this slice exists so that Phase 3 — the largest slice in the program — does
not start with an inherited to-do list attached to it.

## Files this slice owns

| File | Change |
|---|---|
| `test/test_lisp.c` | Extend the existing real-editor fixtures for `buffer-list` and backward regexp search. |
| `test/pty/lisp-process-status.yaml` | Exercise live, normally exited and signalled process states through real tmux-backed children. |
| `test/lisp-compat/features.json` and the three matching `cases/*.json` notes | Close the recorded gaps and remove stale “untested” prose without changing the case expressions. |
| `lisp/prelude.el` and `src/lisp_prelude_generated.inc` | Settle the three quote-nil workarounds; the include changes only through the generator. |
| `test/test_perf.c` and `utils/bench.py` | Add matching intent comments above the two deliberate numeric comparisons. |

No production native implementation changes unless one of the new tests
finds a real defect; such a fix is its own focused commit.

## Item 1 — the three untested kg natives

`test/lisp-compat/features.json` records three natives whose `kg_test`
field begins `GAP:`, recorded by sub-plan 00C on 2026-08-04 with owner kg
and no phase assigned:

| Feature id | Native | Where implemented |
|---|---|---|
| `native-buffer-list` | `(buffer-list)` | `src/lisp_obj.c` |
| `native-re-search-backward` | `(re-search-backward ...)` | kg's search adapter over `kg_regex_match_backward()` |
| `native-process-status` | `(process-status ...)` | kg's process adapter |

None was found broken; 00C's rationale for each says so explicitly, and
that judgement is not being revisited here.  What is missing is a test that
would notice if one broke.

**Write the cheapest test that actually exercises the native**, per the
project's own rule about which harness to use.  The harness question is
settled by the current tree; do not rediscover it while implementing:

- Extend `test/test_lisp.c:test_buffer_objects` for `buffer-list`.  That
  test already uses `setup_editor()` plus the real buffer manager, creates
  a hidden buffer with `get-buffer-create`, and compares real Fe buffer
  objects.  Assert all three parts of the native's contract: the current
  buffer is first, each live buffer occurs once, and the created hidden
  buffer is present.  Do not add a PTY case merely because the result type
  is an editor object; this unit fixture is not `stubs_lispobj.o`'s fake.
- Extend `test/test_lisp.c:test_re_search_and_match_data` for
  `re-search-backward`.  Give the row at least two possible matches, start
  at `point-max`, and assert the chosen match's start plus group 0/1
  `match-beginning`/`match-end`.  Add a bound/no-match assertion that point
  and the previous match data behave exactly as the current
  `lisp_search()` contract says.  This deliberately pins kg's
  `kg_regex_match_backward()` selection policy; it is not an Emacs
  differential case.
- Add `test/pty/lisp-process-status.yaml`, `backend: tmux`, for
  `process-status`.  Record `'run` immediately for a deliberately
  long-lived child, and record `'exit` and `'signal` from sentinels for a
  normally exiting child and a self-signalling child.  Have a later bound
  command insert those saved symbols into the case's file, following
  `lisp-process-filter-sentinel-order.yaml`'s safe-point/drain pattern.
  Keep `key_delay` and the finite number of drain keypresses explicit.  Do
  not use a blind `sleep` in the harness or assert callback order unless
  the case actually needs it.

Then update the same three entries in
`test/lisp-compat/features.json`: replace `kg_test`, change `status` from
`unsupported` to `supported`, and clear the gap-only `rationale` to `null`.
Remove “untested” wording from the matching case-file `note` fields and
name the owning kg test there; do not change their expressions or pretend
the kg-policy JSON is an executable editor fixture.  Do not delete the
entries or their `cases`; they are the census.  Run
`make lisp-compat-check` after editing -- it validates the complete entry,
not just the path string.

If a native turns out to be broken after all, that is a finding, not a
scope expansion: fix it in its own commit, with the test that found it, and
record it.

## Item 2 — the three `(list 'quote nil)` prelude workarounds

`lisp/prelude.el`'s header (lines 31–41 in the audited tree) already states
that the rule licensing them — "no macro may expand to bare nil" — is
obsolete, and 01A verified that against `fe.c`'s `FeTMacro` arm.  Three
uses survive:

| Line | Form | Role |
|---|---|---|
| 141 | `cond` | the no-more-clauses base case |
| 260 | `defvar` | the "already bound, do nothing" branch |
| 266 | `interactive` | the whole body — a deliberately inert macro |

01A left them under its own "if in doubt" route, and 02D deleted only the
fourth (in the now-deleted `setq` macro).

**Decide, once, with evidence, and write the answer into the file.**  The
evidence is one experiment, not an argument: replace each `(list 'quote
nil)` with bare `nil`, run `make lisp-prelude-generate`, and check that

- `make check` is green at the runner's newly discovered count (the
  pre-slice baseline is 32 native suites / 405 PTY cases, and item 1 adds
  one PTY);
- `test/test_lisp.c:test_prelude_source_file`'s definition-order pin still
  holds;
- `make lisp-prelude-check` reports the generated include current, and the
  diff of `src/lisp_prelude_generated.inc` contains only those three source
  substitutions; and
- the specific behaviours these guard still hold — `(cond)` with no
  matching clause is nil, `defvar` on an already-bound variable does not
  reassign, and a top-level `(interactive)` is inert **and** returns
  something that a surrounding `progn` treats as nil.

The named behaviour checks already exist in
`test/test_lisp.c:test_prelude_forms` and `test_definition_forms`; extend
those functions only if the experiment exposes a missing assertion.  In
particular, do not create three duplicate PTY cases for pure prelude
semantics.

If all four hold, delete the workarounds and rewrite the header paragraph
to say they are gone.  If any fails, keep the workaround, and replace the
header's "vestigial rather than wrong" wording with the concrete failure —
a named form and what it evaluated to.  Either way the file stops carrying
a hedge.

`interactive` deserves particular care: it is the one whose *return value*
is load-bearing at every `defun` site that uses it, and it is also the one
Phase 7 rewrites when interactive specs become real.  If the experiment is
ambiguous for `interactive` alone, keeping that one and deleting the other
two is a legitimate outcome — say which and why.

## Item 3 — the two documented `(= X Y)` survivors

`test/test_perf.c:992` and `utils/bench.py:119` both contain

```lisp
(defun my-before-save-hook () (= my-fill-column my-fill-column))
```

which after Phase 2 is a legitimate numeric comparison whose value is
discarded — a hook body that does a trivial amount of work, which is all
the benchmark needs.  It is not a missed assignment migration, and the set
README says so, but the README is not where someone auditing
`grep -rn '(= '` will look.

Add a one-line comment immediately above the form at each site saying it is
a numeric comparison on purpose and that the return value is deliberately
discarded.  Keep the two comments equivalent so a later text audit finds
both.  That is the whole item; do not change the benchmark workload.

## Tests owned by this slice

- Two focused assertions in existing `test/test_lisp.c` tests and one new
  `test/pty/lisp-process-status.yaml` case, together covering one native
  per gap.
- No new test for items 2 and 3: item 2's proof is the existing suite plus
  the four behaviour checks listed, and item 3 is a comment.

## Gates

Rule 9 applies unchanged: `make check` and
`make WITH_LISP=0 clean all check`, both from an idle tree.  The expected
numbers are **32 native / 405 PTY** and **337 pass + 68 skip**, plus
whatever this slice adds; anything else means build contention until proven
otherwise.

`make lisp-compat-check` must stay green — it is the gate that reads the
manifest fields this slice edits.  `make lisp-prelude-check` must stay
green if item 2 changes the prelude.

Complexity: kg's scc should not move (this is Lisp, YAML and comments,
plus at most a C test file, which is not scanned by
`SCC_COMPLEXITY_PATHS`).  Record it anyway — the tree is at **5444/5500**,
measured 2026-08-04.  Fe is untouched at **214/220**.

This slice does **not** need `.ci/run-ci-steps.sh --parallel` on its own;
it is not a workstream.  It is, however, a fine thing to fold into the
first full run Phase 3 does.

Do **not** run or regenerate the Emacs oracle snapshots.  All three natives
are `comparison: kg-policy`, the prelude experiment is already covered by
the checked-in unit expectations, and no language answer is changing.

## What this does not do

- **No new natives, and no fixes to the three unless a test finds one
  broken.**  00C already judged them implemented and plausible; this is
  test-writing, not a rewrite.
- **No prelude restructuring.**  Item 2 deletes three forms or keeps them;
  it does not reorder definitions, which `test_prelude_source_file` pins
  deliberately.
- **No audit tooling.**  Item 3 is two comments.  A checker that
  distinguished "comparison meant as comparison" from "assignment that was
  never migrated" would have to read intent, and Phase 2's set already
  ruled that out.

## Status

**Complete, 2026-08-04.**  All three items closed in three focused kg
commits, no fe change, no pin move.  `make check` (32 native / 406 PTY,
one more PTY than the 32/405 baseline, exactly the "item 1 adds one PTY"
the Gates section predicted), `make WITH_LISP=0 clean all check` (337
pass + 69 skip, one more skip than baseline -- `lisp-process-status.yaml`
is `requires_feature: lisp`), `make lisp-compat-check` (181 features
across both manifests, 0 problems) and `make lisp-prelude-check` are all
green from an idle, fully clean tree at the final commit.  Complexity is
unmoved on both ratchets, measured before starting this slice and again
at the end: kg scc **5444/5500** (unchanged), kg pmccabe **1246 symbols
recorded, 1246 measured, 0 new, 0 gone, 0 improved** (unchanged); fe is
untouched at **214/220** (`fe.c` 106/112).  None of it needed
`.ci/run-ci-steps.sh`, per this slice's own scope.

**Item 1 (`37be96a`).**  None of the three natives was broken -- 00C's
2026-08-04 judgement holds unrevisited, exactly as this document
predicted.  `buffer-list` and `re-search-backward` each got two to six
focused `CHECK()`s added to the existing real-editor `test_lisp.c`
fixtures named in the "Files this slice owns" table, no new test
function for the former, one new `test_re_search_backward` function for
the latter (a second gap on the same native fixture would have padded an
unrelated test with an unrelated concern).  `re-search-backward`'s
hand-derived offsets (second `\(foo\)bar` match in "foobar foobar",
searched backward from `point-max`: return value and `match-beginning 0`
both 8, `match-end 0` 14, group 1 8/11) passed on the first build, which
is itself a small piece of evidence that `kg_regex_match_backward()`'s
documented "last match ending at or before the limit" policy is exactly
what the code does.  `process-status` got the new PTY case
`test/pty/lisp-process-status.yaml`: one child immediately queried while
still running (`'run`, read synchronously right after
`start-shell-command`, no drain needed), two children whose sentinels
read `(process-status p)` off their own first argument once terminated
(`'exit` from `true`, `'signal` from `kill -KILL $$`) -- sidestepping any
question about whether a `let*`-bound process handle is visible inside a
later sentinel closure, since the sentinel's own first argument already
names the process. 8/8 runs green (3 required by the operating rules,
5 more for margin), `--jobs 1`.  All three manifest entries moved
`unsupported` → `supported` with `kg_test` naming the real test and
`rationale` cleared to `null`; the three case-file `note` fields drop
"untested" wording and name the owning test, per the slice's own "do not
change expressions" instruction.

**Item 2 (`336afcf`).**  Ran the actual experiment rather than asserting
an outcome: replaced all three surviving `(list 'quote nil)` forms (in
`cond`, `defvar`, `interactive`) with bare `nil`, regenerated, and checked
all four things the sub-plan named.  All four held --
`make check` at the newly-discovered 32/406, `test_prelude_source_file`'s
ordering pin, `make lisp-prelude-check` green with the generated `.inc`
containing only those three substitutions (confirmed by decoding both the
old and new checked-in byte arrays back to Lisp source and diffing that,
not the checked-in file's own diff -- a flat `unsigned char[]` printed 16
bytes/line reflows its *entire remainder* on any length change, so a
three-line source diff is a several-hundred-line diff in the `.inc`;
01A's own Status section anticipated exactly this by decoding for its
proof, and this slice reused that method rather than trusting `git diff`
on the generated file) -- and the three named behaviours already pinned
in `test_prelude_forms`/`test_definition_forms` (`(cond)` is nil,
`defvar` does not reassign an already-bound variable, top-level
`(interactive)` reads as nil), unchanged by the swap and requiring no new
assertion.  So the workarounds are gone, not kept: `lisp/prelude.el`'s
header now states the four-workaround history and the four things that
were checked, replacing the "vestigial rather than wrong" hedge 01A left.

**Item 3 (`5644f14`).**  One comment above the form at each site
(`test/test_perf.c`, `utils/bench.py`), worded equivalently so a later
`grep -rn '(= '` audit finds both, exactly as specified.  The Lisp
workload string itself is byte-for-byte unchanged at both sites --
verified by diff, since a changed workload would have silently moved
`lisp-arena-representative-init`'s baseline.

### What this document got wrong

Nothing in the three items' outcomes: the baseline counts, the "item 1
adds one PTY" prediction, the "no scc movement expected" prediction, and
the open-ended item 2 experiment (which the document explicitly refused
to prejudge) all came out exactly as written or exactly as left open.

One thing the document didn't anticipate, found while iterating on item
2: **`make`'s dependency graph does not know `src/lisp_prelude.o` depends
on `src/lisp_prelude_generated.inc`.**  There is no `-MMD`/`-MP` or
`%.d`-style auto-dependency tracking in this Makefile, so
`make lisp-prelude-generate` followed by a bare `make`/`make check` can
report a fully green run against a *stale* `src/kg` that never recompiled
`lisp_prelude.c` -- `lisp-prelude-check` validates the generated file
against the source, not that the running binary embeds it. It cost
nothing here (a `make clean` was already the right move to re-baseline
complexity across the slice, and the discrepancy was only a header
comment's byte count, not an evaluated form), but the same gap would
silently pass a `make check` after a *behaviour*-changing prelude edit
run without an intervening `make clean` or a touch of the generated file.
Not fixed here -- out of this slice's scope -- but worth a line for
whichever future slice next edits `lisp/prelude.el` under time pressure.

**Phase 3 -- the frame machine -- remains the next set**, per the set
README's Grouping and Sequencing sections; `03a` is unaffected by
anything in this slice and was already clear to start in parallel.
