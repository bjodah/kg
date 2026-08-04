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
project's own rule about which harness to use:

- `buffer-list` returns real buffer objects, so it needs real buffers.  A
  `test/pty/*.yaml` case that visits two files and asserts on a
  `buffer-list`-derived string is the honest one; a unit test against the
  `stubs_lispobj.o` stub would assert the stub.
- `re-search-backward` is pure logic over a buffer's text.  `test_lisp.c`
  can drive it if the fixture buffer is real there; if the existing Lisp
  unit harness cannot produce buffer text, use a PTY case rather than
  widening the stub set.
- `process-status` needs a live child, and the project notes are explicit
  that real-child async output needs `backend: tmux`.  Expect this one to
  be the slowest and the only one that can flake; if it joins the standing
  `lisp-process-*` flake class, say so in the Status rather than tuning
  delays until it is quiet.

Then **replace each `GAP:` string with the real test's path**, so
`utils/check_lisp_compat.py` keeps carrying an accurate inventory.  Do not
delete the entries; they are the census.

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
nil)` with bare `nil`, and check that

- `make check` is green (32 native suites, 405 PTY cases);
- `test_prelude_source_file`'s definition-order pin still holds;
- `make lisp-prelude-check` regenerates identically; and
- the specific behaviours these guard still hold — `(cond)` with no
  matching clause is nil, `defvar` on an already-bound variable does not
  reassign, and a top-level `(interactive)` is inert **and** returns
  something that a surrounding `progn` treats as nil.

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

Add a one-line comment at each site saying it is a comparison on purpose.
That is the whole item.

## Tests owned by this slice

- The three new native tests above (one per gap), each named for the
  native it covers.
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

Not started.
