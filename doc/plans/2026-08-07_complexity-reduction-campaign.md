# Deduplication and complexity-lowering campaign (CX)

Status: CLOSED 2026-08-07 by a 12/12 parallel CI run (see Results at the
end).  Both prerequisites had landed the same day (FreeBSD at ad53e28,
kill-ring-in-prompts at c950793, each closed by a 12/12 run of its own).

Origin: `2026-08-06_complexity-reduction-potential.md` (findings doc).
Its central claim was re-verified on 2026-08-07: `lizard -m` (modified
McCabe, a multi-case `switch` counts as one decision, matching pmccabe's
first column) reproduces the pmccabe baseline number-for-number, and the
functions that survive that correction are genuinely branch-heavy rather
than switch-shaped. Re-measure before starting (numbers below WILL have
drifted; the baseline file is the source of truth):

```
lizard -m src/*.c --Threshold cyclomatic_complexity=40 --warnings_only
jq -r '.functions | to_entries[] | select(.value >= 35) | "\(.value)\t\(.key)"' \
    .ci/pmccabe-baseline.json | sort -rn
```

Measured 2026-08-07 (kg 9cdefce):

| symbol | pmccabe = lizard -m |
|---|---|
| src/localvars.c:localvars_parse_footer | 91 |
| src/display.c:draw_window_rows | 67 |
| src/basic.c:editor_move_cursor | 61 |
| src/word.c:editor_reflow_paragraph | 59 |
| src/syntax.c:generic_keyword_scan | 54 |
| src/localvars.c:localvars_parse_modeline | 51 |
| src/search.c:do_isearch | 49 |

## Charter

Behavior-frozen refactoring: no user-visible change, no keybinding
change, no divergence-manifest change, no new oracle snapshots. The
deliverable is lower banked pmccabe entries and less duplicated
machinery, proven by the existing suites staying green unchanged.
"Extract enough ifs to satisfy pmccabe" is explicitly NOT the goal —
each extraction must be a nameable stage or a shared scanner that a
maintainer would want even if no ratchet existed. Moving branches into
a helper solely so they stop counting (the `flat_row_advance()` shape,
which documents itself as exactly that) is the anti-pattern this
campaign must not add more of.

## Sub-plans, in order

### CX-A: the localvars pair (strongest candidate)

`localvars_parse_footer` (91) and `localvars_parse_modeline` (51) share
hand-rolled machinery: quote/escape-aware scanning for separators,
whitespace trimming, case-insensitive marker search, and apply-dispatch.
The same file already demonstrates the target style — the `dlr_*`
reader (`dlr_skip_ws` / `dlr_read_sym` / `dlr_read_str` /
`dlr_skip_sexp` / `dlr_apply_pair`) is a decomposed parser whose worst
symbol is 35. Attack the pair together:

1. Extract shared slice/scanner helpers first (dedup — this is where
   net line count should FALL), then decompose footer into its stages:
   tail-window build, form-feed cut, line split, `Local Variables:`
   block discovery, prefix/suffix strip, assignment parse,
   continuation-string assembly, apply.
2. Cover: `test_localvars.c` (extend for each new helper's edge cases),
   `fuzz_localvars` with tracked seeds — run it with a raised
   `FUZZ_MAX_TOTAL_TIME` (e.g. 60 s) locally after the refactor, plus
   the normal smoke budget in CI.

### CX-B: editor_reflow_paragraph (easiest high-value)

59, one function: paragraph bounds, git-commit policy, indent
detection, word-stream construction, wrap, serialize, apply. Manual
memory management (`words`/`indent`/`joined`/`new_lines`/`cur` with an
`ok` flag) is half the branch count. Constraints that must survive:
the whole operation stays ONE undo record, and `test_perf`'s
one-`editor_update_row()`-per-logical-replacement shape must not
regress. Cover: `test_word.c`, existing reflow PTY cases.

### CX-C: editor_move_cursor (worthwhile, delicate)

61: wrapped visual lines, logical lines, UTF-8 glyph stepping,
horizontal scroll, rectangle virtual space, desired-visual-column
preservation, four directions. Decompose by MODE (visual-line handled
first, returning "handled"; then left/right/vertical helpers), not by
extracting conditionals. Read `doc/coordinates.md` first; state the
coordinate space at every new seam with
`KG_ASSERT_CHARS_OFF`/`KG_ASSERT_RENDER_OFF` (armed in ci-04). Cover:
`test_basic.c` plus the movement PTY cases including the Emacs-oracle
ones — the oracle is the real referee here.

### CX-D: reassess, then optionally

- `generic_keyword_scan` (54): medium priority. The three
  almost-identical non-decimal number arms (b/B, o/O, x/X) are real
  duplication; but a single stateful scanner loop can beat six helpers
  passing `p, i, prev_sep` around — only proceed if the extraction
  reads better, not just scores better. Note it was raised +2 by the
  Phase 12 funded decision; re-measure.
- `do_isearch` (54 since c950793 banked +5 for its C-y/M-y branches;
  49 at this plan's writing): kill-ring-in-prompts has landed, so the
  sequencing constraint is gone — but reassess its shape first, since
  that landing added the yank branch and the factored `isearch_yank()`
  helper is already in the file's decomposed style.

## Non-goals (do not touch)

- `draw_window_rows` (67, 14 params): deferred. The right fix is a
  render-context struct plus a real structural decomposition, designed
  with bench/perf-counter evidence (`make bench`, `test/perfobj/`) —
  it is a hot repaint loop with coupled terminal state. Out of this
  campaign's scope; leave a TODO.md note instead if not already there.
- `parse_escape` (pmccabe 37): the lizard 63 is terminal-key case
  labels. A table-driven CSI decoder is a functional-design idea, not
  a complexity emergency.
- Anything under `fe/` or `fe/tiny-regex-c/`: `DispatchPrimitive`
  (pmccabe ~9; the 70 is a dispatch switch) and `test_api.c:main`
  (a 74-term `&&` chain of test calls) are metric artifacts, and the
  submodules have their own upstream discipline (Rule 10 pin flow).
  This campaign is kg-side only.

## Ratchet and gate policy

- **pmccabe**: every sub-plan must DECREASE the entries it touches and
  bank them (`make pmccabe-baseline`) in the same commit, with
  before→after numbers in the commit body. New helpers must measure
  ≤15 (the new-function rule) — if a helper needs more, the
  decomposition boundary is wrong.
- **scc (5845 cap)**: extraction adds signature/brace lines; dedup
  removes them. Target net ≤ 0 per sub-plan, with CX-A expected
  clearly negative. If a sub-plan cannot land net-negative-or-zero,
  that is a funded-decision raise: measured before/after and the
  reason in the cap comment and commit body, per the file's existing
  precedent. Never leave slack — cap equals actual at each close.
- **Coverage ratchet**: refactoring shuffles lines; `make coverage &&
  make coverage-check` at every src-touching commit (then `make clean`
  — coverage instruments objects in place; clean stale fe gcov files
  if lcov complains).
- **Gateway census** (`make gateway-check`): counts must not rise; no
  new file may appear in the manifest. Keep extractions in-file.
- **Per commit**: `make check`, `make complexity-check`,
  `make pmccabe-check`, `make format-check` — exit-status-checked.
- **Final green light**: `JOBS=8 .ci/run-ci-steps.sh --parallel`,
  12/12. Known flakes: ci-05 MSan test_compile SIGINT (pre-existing),
  ci-03 valgrind lisp-process-cwd (budget already doubled). A failed
  lane is re-run standalone and BOTH observations recorded honestly.

## Standing rules (bind the executing agent)

Never push anywhere. Commits end
`Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. The two
stashes on "WIP on stricter-emacs-adherence: f05af25" are the user's —
untouched. `/work/launch.sh` stays. Do not edit other files under
`doc/plans/` (appending a short results section to THIS file at close
is allowed). Re-measure everything; never carry a figure from this
plan or the findings doc into a commit body without re-running the
tool. No behavior flips: if a PTY case or oracle snapshot disagrees
after a refactor, the refactor is wrong — fix the code, never the
expectation.

## Report back

Per sub-plan: commit SHAs, the banked pmccabe before→after per symbol,
net scc delta, gate exit statuses, and the final 12/12 run. Plus a
one-paragraph honest assessment per extraction: did it make the code
better, or just the number smaller?

## Results (closed 2026-08-07)

Status: CLOSED.  All four sub-plans landed; CX-D took the campaign's one
funded scc raise.  Commits, on `stricter-emacs-adherence` on top of
c5913cd:

| | commit | symbol | pmccabe | scc |
|---|---|---|---|---|
| CX-A/1 | 07ebbfc | `localvars_parse_modeline` | 51 -> 12 | -32 |
| | | `localvars_parse_footer` | 91 -> 63 | |
| CX-A/2 | 8c3560f | `localvars_parse_footer` | 63 -> 9 | 0 |
| CX-B | cc4c0d9 | `editor_reflow_paragraph` | 59 -> 8 | -3 |
| CX-C | c00dfbb | `editor_move_cursor` | 61 -> 12 | -7 |
| CX-D | 9f322ea | `generic_keyword_scan` | 54 -> 48 | +4 |
| | | `do_isearch` | 54 -> 40 | |
| fix | c649e68 | `footer_read_string` | 10 -> 12 | 0 |

scc total 5879 -> 5841 over the campaign, the cap re-set at the measured
actual at each close.  Every new helper measured at or under the 15
new-function ceiling; the worst is `footer_build_window` at 13.  Final
green light: `JOBS=8 .ci/run-ci-steps.sh --parallel`, 12/12 -- on the
second run.  The first was 11/12: ci-06's clang-analyzer found that
CX-A/2 had moved the footer's continuation reader out of sight of the
proof that a value length is non-negative, fixed in c649e68 with the
bound test spelled out.  Nothing else failed on either run, and neither
of the known flakes (ci-05 MSan test_compile, ci-03 valgrind
lisp-process-cwd) appeared.

Two bugs fell out of the refactoring, both pre-existing:

- `localvars_parse_footer()` read a body of *negative* length when a
  block's comment prefix and suffix overlap ("aaa"/"aaa" against the
  line "aaaa") and handed it to `memcpy()`.  Reproduced on the tree
  before the change under ASan (`negative-size-param: (size=-2)`), fixed
  by the shared `footer_line_body()` guard in CX-A/2 and pinned by
  `test_footer_overlapping_prefix_suffix_no_body`.
- The same function's continuation reader skipped the prefix/suffix
  check on the line it was already standing on -- one of three
  hand-written copies of that test having drifted from the other two.
  Benign, because the caller had already checked it.

Two things the campaign declined to do, on purpose:

- The rest of `generic_keyword_scan()` (48).  It is one stateful loop
  over `(p, i, prev_sep, in_string, in_comment)`; six helpers passing
  that tuple by pointer would read worse than the loop, which is the
  test this plan set for it.  Only the triplicated 0b/0o/0x arms were
  dedup'd.
- `do_isearch()`'s key-dispatch ladder (the function is 40).  Its arms
  are already two to five lines over the file's existing `isearch_*`
  helpers; the two things that were *not* already factored --
  the search step and the ESC restore -- are.

One finding worth carrying forward: on CX-D, scc and pmccabe disagree
about the direction of the change, and scc is the one that is wrong for
this kind of work.  Measured on the tree: adding the radix table with
the three duplicated arms *still in place* scores the same +3 as adding
it and deleting them, so a 50-line deletion earns exactly zero credit
and the whole charge is the helper's own existence.  The dedup written
as a switch per radix measured +7 and as an if-chain +21.  scc counts a
function's braces and its branch keywords; it cannot see copies that
went away.  Any future plan that budgets a dedup campaign against
`SCC_COMPLEXITY_MAX` should expect to pay rather than earn, and should
price it from the per-symbol pmccabe manifest instead.
