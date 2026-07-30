# Regex correctness, safety, and performance review

Date: 2026-07-30
Scope: `fe/tiny-regex-c`, kg's `src/regex.c` wrapper and search/replace
callers, Fe's `fex_re.c` binding, and the associated tests/fuzzers.
Method: source review, focused kg/Emacs differential probes, the native regex
suite, and a 20,000-case seeded differential run. No product code was changed.

## Executive summary and top five priorities

1. **Fix zero-width regexp replacement progress on UTF-8 immediately
   (high severity).** `query-replace-regexp` advances one byte after an empty
   match. The wrapper then snaps a mid-glyph match backwards, so `!` can loop
   forever at the same character; an empty replacement spins without another
   key read, while a non-empty replacement can grow the line indefinitely.
2. **Remove the semantic 256-repeat ceiling for groups (high).** The compiler
   accepts counts through 65,535, but the matcher silently returns no match for
   a consuming group repeated 257 or more times. This is a confirmed
   kg/Emacs disagreement and is reported as ordinary `NOMATCH`, not
   `TOO_COMPLEX`.
3. **Preserve and surface matcher exhaustion (medium-high).** tiny-regex-c has
   a distinct `RE_STATUS_TOO_COMPLEX`; kg collapses it into `KG_REGEX_NOMATCH`.
   Searches and replacements can therefore silently miss real matches.
4. **Make the parser reject malformed syntax and define the supported Emacs
   dialect (medium).** Unterminated nested groups and bracket expressions are
   accepted, and leading/repeated quantifiers disagree with Emacs. These cases
   are outside or discarded by the current differential generator.
5. **Replace global/recursive execution state with a per-call execution
   context and explicit work stack (medium architectural priority).** This
   makes matching reentrant, makes resource limits honest, removes the
   arbitrary group cap, and is the cleanest base for richer Emacs regexp
   features.

The existing work is materially better than the original tiny-regex-c design:
spans are bounds-checked, UTF-8 atoms are stepped as glyphs, captures
backtrack, and total recursive work has a ceiling. The native suite passed,
and:

```text
REGEX_DIFF_CASES=20000 make check-regex-differential
regex differential: cases=20000 diverged=0 incomparable=0 seed=20260729
```

That green result applies to the generator's deliberately narrow valid-pattern
grammar, not to the confirmed gaps below.

## Confirmed findings

### R1. Empty-match query replacement can spin forever or grow memory on UTF-8

- **Severity:** High
- **Confidence:** High (deterministic control-flow trace; directly supported by
  an existing wrapper test)
- **Evidence:**
  - `src/search.c:948-953` advances `match_col` by exactly one **byte** after a
    zero-length match, whether accepting or skipping it.
  - `src/regex.c:41-53` snaps an empty match whose start is inside a UTF-8
    glyph back to that glyph's first byte.
  - `test/test_regex.c:420-423` explicitly locks in that behavior: matching
    `"."` from byte offset 1 inside `å` returns a span beginning at byte 0.
  - In replace-all mode, `src/search.c:916-918` synthesizes `y` and reads no
    further key, so `C-g` cannot interrupt the loop.
- **Minimal interactive reproducer/test idea:** create a one-character file
  containing `å`; invoke `query-replace-regexp`; search for `a*`; use an empty
  replacement; press `!`. First match is empty at byte 0. The caller advances
  to byte 1, the continuation byte; the next engine result is snapped back to
  byte 0, and this repeats forever. With replacement `x`, each iteration
  inserts another `x` immediately before `å` and again lands on its continuation
  byte.
- **Fix direction:** progress by one complete subject glyph from a stable
  pre-edit position, not by one byte. More fundamentally, reject or normalize
  a mid-glyph `start_offset` before calling the engine and enforce the contract
  that every returned match starts at or after the requested offset. Add a PTY
  regression with a timeout and an explicit saved result for both empty and
  non-empty replacements.

### R2. Valid consuming groups repeated more than 256 times never match

- **Severity:** High
- **Confidence:** High (executed kg/Emacs reproducer)
- **Evidence:**
  - The parser accepts interval bounds through 65,535:
    `fe/tiny-regex-c/re.c:61-69` and `:511-567`.
  - Group execution has a separate hard ceiling of 256:
    `fe/tiny-regex-c/re.c:71-78` and `:1501-1513`.
  - Hitting it merely returns `NULL`; it does not mark the run too complex.
    The result becomes ordinary no-match through `:1680-1693` and
    `:447-451`.
- **Executed reproducer:**

  ```sh
  python3 -c 'print("\\(a\\)\\{300\\}\t" + "a"*300)' |
      ./test/regex_differential
  # nomatch

  python3 -c 'print("\\(a\\)\\{300\\}\t" + "a"*300)' |
      TERM=xterm-256color emacs -Q --batch -l utils/regex_oracle.el
  # match 2 0 300 299 300

  python3 -c 'print("a\\{300\\}\t" + "a"*300)' |
      ./test/regex_differential
  # match 1 0 300
  ```

  The single-atom form succeeds, isolating the bug to group repetition.
- **Fix direction:** implement group repetition with an explicit work stack or
  iterative repetition frames. A resource limit may return `TOO_COMPLEX`, but
  must not silently change the accepted language. Add boundary tests at
  255/256/257 and at a larger count, for consuming and empty groups.

### R3. kg turns matcher exhaustion into “no match”

- **Severity:** Medium-high
- **Confidence:** High
- **Evidence:**
  - `re_exec` returns `RE_STATUS_TOO_COMPLEX` when its two-million-step/depth
    budget is exhausted (`fe/tiny-regex-c/re.c:80-101`, `:1625-1636`, and
    `:447-451`).
  - `kg_regex_match_forward` maps every non-OK status to
    `KG_REGEX_NOMATCH` (`src/regex.c:124-130`); backward matching does the same
    at `src/regex.c:150-154`.
  - The differential driver has to bypass the wrapper expressly because of
    this loss (`test/regex_differential.c:38-45`).
  - Both incremental search (`src/search.c:112-128`) and regexp replacement
    (`src/search.c:866-873`) consequently treat exhaustion as absence.
  - Fe's direct binding correctly retains the distinction
    (`fe/fex_re.c:167-184`), so the same engine has inconsistent public
    behavior depending on caller.
- **Reproducer/test idea:** promote the existing `exec_ran_out` concept into a
  unit case that asserts the raw engine returns `TOO_COMPLEX`, then assert the
  kg wrapper exposes a corresponding status and the UI shows an explicit
  “regexp too complex” message instead of “no match.”
- **Fix direction:** add a kg runtime-complexity status distinct from compile
  failure/no-match and thread it through both search call sites. Cancellation
  should also be polled during long matches; a fixed work ceiling is useful,
  but a false-negative search is not an acceptable presentation of it.

### R4. The compiler accepts malformed Emacs regexps

- **Severity:** Medium
- **Confidence:** High (executed differential probes)
- **Evidence:**
  - On `\(`, the parser only scans ahead for *some* later `\)` at
    `fe/tiny-regex-c/re.c:664-691`; it never verifies that every opened group
    was closed before writing the sentinel at `:831-845`.
  - `compile_charclass` reaches NUL and still succeeds
    (`fe/tiny-regex-c/re.c:470-505`), so an unterminated non-negated class is
    accepted.
  - `*`, `+`, and `?` only check `j > 0` (`:619-635`), unlike interval parsing,
    which uses the more accurate `quantifiable()` at `:570-580`.
- **Executed examples:**

  ```text
  pattern       subject   kg                         Emacs
  \(\(a\)       a         match 3 0 1 0 1 0 1       badpat
  [a            a         match 1 0 1               badpat
  []            x         nomatch                   badpat
  *             *         badpat                    match 1 0 1
  ^*            *         match 1 0 0               match 1 0 1
  a++           aa        nomatch                   match 1 0 2
  ```

- **Fix direction:** use a real compile-time group stack and explicit parser
  states for bracket expressions and quantifiers. Decide whether the public
  goal is an Emacs-compatible subset or merely a documented independent
  dialect; for an Emacs-style editor, accepting a deliberately bounded subset
  but matching Emacs exactly within it is the less surprising contract.
  Invalid-pattern cases must be compared, not discarded, by the differential
  suite.

### R5. Execution is not reentrant or thread-safe

- **Severity:** Medium as a library/API defect; low for today's single-threaded
  kg
- **Confidence:** High
- **Evidence:** `re_match_steps` and `re_match_depth` are process-global
  writable variables (`fe/tiny-regex-c/re.c:294-298`) reset on every match at
  `:1665-1666`. A nested or concurrent `re_exec` corrupts both calls' budgets.
  The legacy `re_compile()` result is also one shared static buffer at
  `:848-857`.
- **Reproducer/test idea:** a two-thread ThreadSanitizer harness repeatedly
  runs one catastrophic and one easy expression. For pure reentrancy, add a
  test-only callback/cancellation hook that performs a nested match; the outer
  counter will be reset.
- **Fix direction:** move counters, limits, cancellation, and the compiled
  program bounds into a caller-owned/per-call context. Deprecate the shared
  `re_compile()` convenience API in favor of `re_compile_checked`.

### R6. Incremental search matches rendered terminal text, not buffer text

- **Severity:** Medium-low correctness/Emacs-affordance gap
- **Confidence:** High
- **Evidence:** regexp isearch passes `row->render` at
  `src/search.c:112-120`; rendering expands each tab into spaces at
  `src/buffer.c:293-317`. Query-replace-regexp instead matches `row->chars`
  at `src/search.c:863-867`.
- **Impact/reproducer:** on a line beginning with one tab, isearch-regexp for
  eight literal spaces can match, while a literal tab cannot be matched as a
  tab and query-replace sees different text. Emacs searches buffer contents,
  not screen expansion.
- **Fix direction:** match `row->chars`, keep all spans in buffer-byte
  coordinates, and convert only for highlighting/reveal. Add PTY coverage for
  tab searches and consistency between isearch and query-replace.

### R7. Regexp replacement performs quadratic row rebuilding

- **Severity:** Medium-low performance
- **Confidence:** High for complexity; magnitude depends on line length
- **Evidence:** each replacement deletes `match_len` bytes and inserts
  `expanded_len` bytes one at a time (`src/search.c:932-942`). Each delete
  memmoves the remaining row and rebuilds render/syntax
  (`src/buffer.c:891-900`); insertion likewise reallocates/moves and updates
  the row (`src/buffer.c:748-775` and the remainder of that function).
- **Test idea:** benchmark replacing a 100 KiB single-line match with another
  100 KiB string; track wall time and `editor_update_row` calls.
- **Fix direction:** introduce one checked `editor_row_replace_range()` that
  resizes/memmoves once and refreshes render/syntax once. The literal
  query-replace path at `src/search.c:690-713` has the same opportunity.

## Test and analysis gaps

These are confirmed properties of the infrastructure, not claims that every
excluded area currently contains another bug.

- The differential grammar limits intervals to 0..5, nesting to two levels,
  subjects to eight characters, and groups to nine
  (`utils/regex_differential.py:55-57`, `:75-101`, `:125-137`). It cannot find
  R2.
- It explicitly excludes repeated quantifiers (`:105-115`), most POSIX
  classes/ASCII-vs-Unicode semantics (`:44-53`), backward matching,
  non-zero/mid-glyph offsets, invalid UTF-8, and case-insensitive execution.
- Most importantly, it labels either kg `badpat`/`toocomplex` or Emacs
  `badpat` as “incomparable” (`:210-217`). That hides parser
  acceptance/rejection disagreements rather than testing them.
- Root CI gives the regex libFuzzer only 50 mutation runs
  (`Makefile:247-248`, invoked by `.ci/ci-09-fuzz-smoke.sh:7-8`). The nested
  tiny-regex-c target defaults to 2,000 runs
  (`fe/tiny-regex-c/Makefile:161-189`) but is not the root CI target.
- The root fuzzer exercises forward and backward calls
  (`test/fuzz_regex.c:54-59`) but asserts no semantic invariants. Add span
  bounds, glyph-boundary, monotonic-start, and forward/backward consistency
  oracles; run a separate structure-aware differential fuzzer for semantics.

Recommended expansion sequence:

1. Add a deterministic corpus for malformed syntax, repeated quantifiers,
   offsets at every byte, group counts around limits, and empty-match
   iteration.
2. Compare compile status exactly. Treat only an explicit allowlist of
   documented dialect differences as incomparable.
3. Generate operation sequences: compile, repeated forward “next,” backward,
   replacement expansion, and mutation of a line after a match.
4. Add `ICASE` on both sides by binding Emacs' `case-fold-search`, plus
   multiline row-level cases for anchors.
5. Run long sanitizer fuzzing out of band and merge minimized crashes and
   semantic divergences into tracked seeds. Keep smoke CI fast, but 50 runs is
   only seed replay in practice.

## Architectural recommendation

Do not continue growing the current flat byte-buffer format plus recursive
continuations one special case at a time if richer Emacs behavior is a serious
goal. Preserve the small public API, but compile into a validated instruction
stream with:

- explicit opcodes and 32-bit indices/counts;
- a compile-time group/alternation stack;
- an explicit execution stack and capture snapshot arena;
- per-execution work/depth/cancellation limits;
- length-bearing pattern/subject APIs, with byte offsets at the boundary;
- one shared UTF-8 decoder contract; and
- a documented syntax/case-fold policy.

That overhaul simultaneously fixes R2/R4/R5, makes `TOO_COMPLEX` reliable,
opens a path to Emacs staples such as non-greedy repetition, word/symbol
boundaries, syntax classes, backreferences, and `regexp-opt`-generated
patterns, and permits safe optimizations (required-prefix search, first-byte
sets, anchored fast paths) before entering the backtracker.

## Hypotheses requiring measurement or a dedicated safety run

- `MAX_MATCH_STEPS` counts `match_seq` entries, not glyph scans or helper
  walks (`fe/tiny-regex-c/re.c:1436-1459`, `:1326-1360`, `:1625-1636`).
  Therefore “two million steps” is not a hard CPU bound. Adversarial
  pattern/subject benchmarks should establish worst-case wall time.
- `MAX_MATCH_DEPTH` is 4,096 C frames (`fe/tiny-regex-c/re.c:91-101`).
  The comment estimates under 1 MiB, but this is compiler/ABI dependent and
  may be unsafe on small-thread stacks. Measure `-fstack-usage` across the
  supported GCC/Clang configurations and replace recursion rather than tuning
  the constant.
- The structural UTF-8 decoder accepts overlong encodings and surrogate/out-of-
  range scalar encodings as multi-byte glyphs (`fe/tiny-regex-c/re.c:198-223`);
  kg's general helper has the same structural-only validation
  (`src/def.h:670-713`). No memory error was observed, but the documented
  “well-formed UTF-8” claim is stronger than the implementation. Decide
  explicitly whether arbitrary bytes, structurally valid sequences, or Unicode
  scalar values define a glyph, then differential-fuzz malformed inputs.

No confirmed out-of-bounds access or use-after-free was found in this bounded
review. That is not a substitute for a sustained ASan/UBSan/MSan fuzz campaign,
especially after parser or execution-stack changes.
