# Quality, static-analysis, and fuzzing roadmap

Date: 2026-07-30
Scope: the combined kg, Fe, and tiny-regex-c verification system: Makefiles,
hosted workflows, `.ci/`, native and PTY tests, fuzzers, differential tests,
coverage, complexity, and portability. This is an additive roadmap; no product
code was changed.

## Executive summary: ranked investments

1. **Make the existing deep pipeline an actual hosted PR gate.** The ten
   numbered `.ci` stages are strong, but the visible GitHub workflow only runs
   `make` and `make check`. Until the deep runner is invoked by hosted CI, its
   results depend on a developer remembering to run it.
2. **Bring Fe and tiny-regex-c's standalone quality suites under the root
   quality umbrella.** Root kg builds only Fe core and the regex engine; it
   does not run Fe's API/extension/script suite, its two focused fuzzers, or
   either subproject's own coverage/complexity gates.
3. **Turn coverage from a report into a ratchet.** Root coverage has no failure
   threshold and does not collect branch coverage. The current generated
   `coverage/src.info` in this tree reports 55.3% lines and 68.2% functions:
   useful baseline data, but no regression prevention.
4. **Build a time-budgeted, invariant-bearing fuzz program.** Root smoke
   budgets are mostly 50 runs; the keypress harness stubs out search and query
   replacement, precisely where a high-risk UTF-8 progress bug was found in
   the companion regex review. Keep smoke cheap, but add scheduled persistent
   corpora, semantic invariants, and stateful search/replace and Fe targets.
5. **Add mutation testing and model/property tests for pure cores.** Line
   coverage cannot show whether assertions detect incorrect results. Targeted
   Mull campaigns and small reference models for buffer/undo, UTF-8 positions,
   regexp iteration, and Fe reader/evaluator behavior will measure test
   strength directly.
6. **Replace coarse complexity ceilings with no-regression baselines per
   function/file.** The root allows a 120-cyclomatic, 641-line function and
   global totals can trade one regression against an unrelated cleanup.
7. **Make portability claims executable.** The current macOS `gcc` matrix cell
   is normally Apple Clang, and there is no musl, FreeBSD, 32-bit, big-endian,
   locale, or terminal-capability lane.

## What already exists

The project is not starting from zero:

- `.ci/ci-01` through `ci-10` cover complexity, coverage, GCC `-fanalyzer`
  plus Valgrind, ASan/UBSan, MSan, clang analyzer/IWYU/cppcheck/clang-tidy,
  formatting, `WITH_LISP=0`, fuzz smoke, and an Emacs regex differential.
- Root `make check` has 17 native test sources and 224 PTY YAML cases. The
  PTY runner supports concurrency, Emacs oracles, explicit xfails, terminal
  dimensions, and two backends.
- kg has four libFuzzer targets (`Makefile:100-110`), Fe has raw-reader and
  grammar-steered evaluator targets (`fe/Makefile:45-62`), and tiny-regex-c
  has its own fuzzer and 20 tracked seeds.
- Complexity is already ratcheted with `scc` and `pmccabe`
  (`Makefile:136-152`); empty pmccabe output correctly fails rather than
  silently disabling the gate (`utils/check_pmccabe_complexity.py:43-53`).
- Fe and tiny-regex-c each have 80% line-coverage thresholds
  (`fe/Makefile:73-77,196-207`;
  `fe/tiny-regex-c/Makefile:93-111`).
- Hosted build CI does exercise Ubuntu/macOS and two compiler command names,
  plus install and Debian packaging (`.github/workflows/build.yml:11-43`).

The goal should be to connect, measure, and deepen this work, not replace it
with a fashionable tool list.

## Confirmed gaps and concrete recommendations

### Q1. The deep `.ci` runner is not wired into the hosted PR workflow

- **Evidence:** `.github/workflows/build.yml:30-33` runs only `make` and
  `make check`. No workflow invokes `.ci/run-ci-steps.sh`. The runner itself
  discovers all numbered scripts at `.ci/run-ci-steps.sh:61` and can isolate
  concurrent builds in throwaway trees (`:283-320`).
- **Risk:** sanitizer, analyzer, fuzz, coverage, complexity, formatting, and
  differential failures can merge despite a green visible PR workflow.
- **Recommendation:** add one Linux `quality` job running:

  ```sh
  ./utils/print-tool-versions.sh
  CI_PARALLEL_LANES=4 .ci/run-ci-steps.sh --parallel
  ```

  Use a pinned OCI/devcontainer image containing GCC, Clang, Emacs, Valgrind,
  lcov, scc, pmccabe, GNU parallel, bear, IWYU, cppcheck, clang-tidy, and
  ccache. Upload `.ci/.run/logs`, `coverage/`, sanitizer logs, corpora changes,
  and analyzer SARIF on failure. Protect the branch on this job.
- **Effort:** Medium (2-4 days including a reproducible image).
- **PR cost:** approximately the wall time of the slowest current lane because
  the runner already parallelizes stages; use path filtering only after
  measuring.
- **Signal:** Very high. This converts existing work into enforcement.

### Q2. Root CI does not test Fe's full supported surface

- **Evidence:**
  - Root builds `fe/fe.c` alone (`Makefile:191-192`); Fe's standalone build has
    `main.c`, `auto.c`, all `fex_*.c`, API/header/example tests, and script
    goldens (`fe/Makefile:26-37,91-97`).
  - Fe has its own complexity, coverage, formatting, IWYU, and two fuzz targets
    (`fe/Makefile:45-87,106-129,179-228`), none called by a root `.ci` script.
  - Root clang analysis can see `fe.c` and `re.c` through the root build
    database, but `cppcheck` is explicitly only `src/*.c`
    (`.ci/ci-06-static-analysis.sh:45-53`) and the unbuilt extensions are
    absent.
  - tiny-regex-c's own workflow runs its tests and CBMC
    (`fe/tiny-regex-c/.github/workflows/c-cpp.yml:17-27`), but a pinned
    submodule's workflow does not run merely because kg checks it out.
- **Recommendation:** add `.ci/ci-11-subprojects.sh`:

  ```sh
  make -C fe check complexity-check pmccabe-check format-check
  make -C fe/tiny-regex-c check complexity-check pmccabe-check format-check
  ```

  Add subproject coverage in the coverage lane and Fe/tiny fuzz smoke in the
  fuzz lane, using separate build directories/copies to avoid object clashes.
  Analyze all Fe `SRCS` and tiny `re.c` with the same analyzer profiles as kg.
- **Effort:** Small-medium (1-3 days; likely includes reconciling tool versions
  and old standalone fixtures).
- **PR cost:** Low-medium; native/script tests are cheap compared with 224 PTY
  cases. CBMC should remain scheduled until its runtime is measured.
- **Signal:** Very high, particularly for `fex_re.c`, process/I/O extensions,
  and the public embedding API.

### Q3. Root coverage is informational, not a gate

- **Evidence:**
  - Root uses `--coverage` but not branch coverage
    (`Makefile:153-156`).
  - It prints `lcov --summary` without `--fail-under-lines`
    (`Makefile:288-307`).
  - It extracts only `src/*.c`, excluding the exact vendored Fe/regex code
    shipped in kg (`Makefile:303-306`).
  - The current artifact reports 3,131/5,665 lines (55.3%) and 174/255
    functions (68.2%).
- **Recommendation:** collect line, branch, and function coverage into
  machine-readable JSON/XML (gcovr is simpler for thresholding; lcov may
  continue generating HTML). Establish:
  - global no-regression baselines at the measured values, never hand-rounded
    upward;
  - per-file baselines, so one well-tested file cannot compensate for a
    regression in another;
  - 85% changed-line coverage for new/modified executable lines via
    `diff-cover`, initially advisory for two weeks then required;
  - explicit exclusion rules only for generated tables, unreachable platform
    shims, and test-only stubs, documented next to the config;
  - separate kg, Fe, and regex reports plus a combined shipped-code report.

  Raise baselines automatically only when main improves; decreases require a
  reviewed baseline change explaining the missing test.
- **Effort:** Medium (2-4 days plus gradual test work).
- **PR cost:** Low; coverage already runs the full suite.
- **Signal:** High once branch/per-file/changed-line ratchets exist.

### Q4. Fuzz smoke is too shallow and important behavior is stubbed away

- **Evidence:**
  - Keypress smoke runs 1,000 iterations, but dirlocals/localvars/regex run
    only 50 (`Makefile:229-259`).
  - Fe's better 1,000-run reader/evaluator smoke
    (`fe/Makefile:51-55,112-129`) is not called by root CI.
  - The kg keypress harness restricts initial rows to short printable/tab text
    (`test/fuzz_keypress.c:51-76`) and routes input through the editor loop,
    which is useful.
  - But its stubs turn regexp search into no-ops
    (`test/fuzz_stubs.c:143-153`) and query replace into a no-op
    (`test/fuzz_stubs.c:195`); prompt reading always cancels
    (`test/fuzz_stubs.c:77-98`). It therefore cannot reach search/replace
    progress, replacement expansion, or minibuffer execution.
  - The regex target calls forward/backward but asserts no invariant
    (`test/fuzz_regex.c:54-61`).
- **Recommendation:** use **time**, not mutation count, as the smoke budget:
  5-10 seconds per target on PRs with `-max_total_time`, seeded from tracked
  corpora. Add:
  1. a safe stateful search/replace target with in-memory prompt answers,
     operation tokens (`search`, `next`, `replace`, `skip`, `all`, `cancel`),
     and an iteration cap treated as a bug;
  2. buffer/undo targets using arbitrary bytes and valid/invalid UTF-8 modes;
  3. Fe GC/object-graph, extension, host-callback/re-entry, and error-unwind
     targets in addition to the existing reader/evaluator pair;
  4. regex assertions: spans in bounds and ordered, no result before a
     normalized offset, backward/forward agreement, replacement iteration
     monotonicity, and raw-engine/wrapper status consistency.

  Run 15-minute ASan/UBSan campaigns nightly, MSan campaigns weekly, merge and
  minimize corpora, and commit only small semantic/crash seeds. Track
  edge/feature coverage, corpus size, exec/s, peak RSS, timeouts, and new
  coverage per week. A scheduled job should fail on a crash/timeout but merely
  report coverage drift until stable.
- **Effort:** Medium-high (1-2 weeks for the first four useful targets).
- **PR cost:** Low after capping at seconds; scheduled cost moderate.
- **Signal:** Very high. This closes known harness blind spots rather than just
  increasing random runs.

### Q5. Differential testing needs exact status and operation-sequence oracles

- **Evidence:** the regex generator only checks forward match at offset zero
  (`test/regex_differential.c:48-71`) and labels kg bad-pattern/too-complex or
  Emacs bad-pattern answers incomparable
  (`utils/regex_differential.py:210-217`). Its grammar deliberately excludes
  several dialect areas (`utils/regex_differential.py:44-57,105-115`).
- **Recommendation:**
  - Split “portable supported subset” from “dialect characterization.”
    Compare compile acceptance exactly in the former; record every divergence
    with a reason code in the latter.
  - Generate forward-next loops, backward matches, every byte/codepoint offset,
    ICASE, zero-width iteration, captures, malformed UTF-8, and replacement
    expansion. Compare whole operation traces with Emacs, not just one match.
  - Run 2,000 fixed cases per PR, rotate one deterministic seed daily, and run
    1-10 million cases scheduled. Store minimized divergences as normal tests.
  - Add Fe differential/property oracles where semantics overlap: reader
    print/read round trips, arithmetic against C/Python for finite values, and
    kg's Emacs-shaped prelude against batch Emacs for a deliberately declared
    pure subset.
- **Effort:** Medium-high.
- **PR cost:** Existing 2,000-case regex run is about 0.2 seconds per project
  notes; scheduled depth is cheap enough to be ambitious.
- **Signal:** High for semantic defects that sanitizers cannot find.

### Q6. Static analysis is broad but configuration and trend data are shallow

- **Evidence:**
  - GCC `-fanalyzer` and Valgrind are combined in one stage
    (`.ci/ci-03-gcc-analyzer-valgrind.sh:7-12`), which obscures whether a
    failure is compile-time or runtime and forces a full analyzer rebuild for
    Valgrind.
  - Clang static analyzer, IWYU, cppcheck, and clang-tidy are present
    (`.ci/ci-06-static-analysis.sh:44-53`).
  - `.clang-tidy:2-23` enables performance/misc and a selective bugprone list,
    but not the full `clang-analyzer-*`, `cert-*`, concurrency, narrowing, or
    many C-specific correctness checks; no `HeaderFilterRegex` is declared.
  - Analyzer output is treated as text, not SARIF, and there is no baseline or
    persistent defect trend.
- **Recommendation:**
  - Split GCC analyzer and Valgrind stages for clearer ownership/timing.
  - Pin tool versions and print them into every log.
  - Add a staged warning profile:

    ```text
    -Wformat=2 -Wformat-overflow=2 -Wshadow -Wundef -Wwrite-strings
    -Wcast-qual -Wcast-align=strict -Wstrict-prototypes -Wmissing-prototypes
    -Wconversion -Wsign-conversion -Wvla -Walloca -Wswitch-enum
    -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wnull-dereference
    ```

    Start in report-only mode, check in narrow line-level suppressions with a
    reason, then turn zero-warning groups into `-Werror` one group at a time.
  - Expand clang-tidy with `clang-analyzer-*`, relevant `cert-*`, and selected
    `bugprone-*`; run `--verify-config`; scope headers explicitly; export
    fixes/SARIF. Use CodeQL's C query suite weekly or on PRs touching C.
  - Run cppcheck with a checked-in suppressions file, `--enable=warning,style,
    performance,portability`, `--inconclusive`, and all compile configurations
    rather than only `src/*.c`.
  - Add semgrep or a small `rg`-based policy check for project-specific hazards:
    byte-wise UTF-8 progress, unchecked `int`/`size_t` conversions, direct
    row mutation without one update, shell execution, and new global mutable
    state.
- **Effort:** Medium, mostly baseline triage.
- **PR cost:** Medium; parallelize by translation unit and run CodeQL weekly if
  it dominates.
- **Signal:** Medium-high after false positives are baselined explicitly.

### Q7. Complexity limits prevent growth but do not target the debt

- **Evidence:** root's total limit is exactly the current 4,208 and its
  function ceiling is exactly 120 (`Makefile:140-152`). Current pmccabe output
  identifies `editor_process_keypress` at complexity 120/641 LOC,
  `localvars_parse_footer` at 100/401 LOC, and `draw_window_rows` at 66/282 LOC.
  The per-file scc ceiling is 520, above today's reported most complex file
  (`bufmgr.c`, complexity 461). Fe's total is 171 against 172 and its
  per-function ceiling 22 against an observed 18; tiny-regex-c's parser is
  complexity 38 against a ceiling of 60.
- **Problem:** one global total can rise in a touched file while an unrelated
  cleanup compensates. A ceiling of 120 prevents a new 121 but does not make
  the outliers easier to test.
- **Recommendation:** check in a generated baseline manifest keyed by
  file/function and fail any increase in:
  - cyclomatic complexity;
  - function LOC/statements;
  - file complexity/LOC;
  - maximum nesting/cognitive complexity.

  Permit decreases automatically. Require a reviewed allowlist entry with
  rationale and expiry for increases. Add staged absolute goals: no new
  function above 15, split existing functions above 40 when substantively
  touched, and reduce the top-three debt monotonically. Use clang's AST or
  `lizard` in addition to pmccabe so attributes do not produce ambiguous names
  such as the current `__attribute__` report.
- **Effort:** Small for manifest enforcement; ongoing refactoring effort.
- **PR cost:** Negligible.
- **Signal:** High for maintainability when paired with tests, not as a proxy
  for correctness.

### Q8. Add mutation and property/model testing

- **Evidence:** existing native tests are expectation-based and coverage is
  line-oriented; there is no mutation target/configuration in the tree.
- **Recommendation:**
  - Use Mull with Clang on fast native binaries, initially `regex`, `localvars`,
    `width`, `buffer`, `undo`, and Fe core. Exclude OOM/error-injection branches
    only with reviewed patterns. Run changed-functions on PRs and full selected
    modules weekly.
  - Seed a mutation-score baseline per module. Fail killed-to-survived
    regressions, not on an arbitrary 100% target. Publish surviving mutants
    with file/line/operator; every fixed survivor becomes a focused test.
  - Add deterministic property tests:
    - insert/delete and undo/redo restore byte-identical buffers and cursor
      invariants;
    - row/flat and char/render conversions are monotonic and round-trip at
      representable positions;
    - UTF-8 movement never lands mid-valid-glyph;
    - formatter/read/print/read round trips in Fe;
    - GC preserves rooted object graphs;
    - regex next-match iteration always advances and spans remain bounded.
  - Use exhaustive small-state enumeration before adding another random
    framework: all strings/operation sequences up to a small bound often have
    better reproducibility and higher semantic density.
- **Effort:** Medium-high (about a week for infrastructure and first module).
- **PR cost:** Changed-function mutation 2-10 minutes; full suite scheduled.
- **Signal:** Very high: it measures assertion effectiveness, not execution.

### Q9. Portability and configuration coverage is narrower than the matrix says

- **Evidence:** hosted matrix names Ubuntu/macOS × gcc/clang
  (`.github/workflows/build.yml:13-21`), but macOS' `/usr/bin/gcc` is normally
  Apple Clang. It does not verify the compiler identity/version. `WITH_LISP=0`
  is checked only in the separate local stage (`.ci/ci-08-with-lisp-0.sh`).
- **Recommendation:** use explicit, verified lanes:
  - Ubuntu: minimum supported GCC and Clang, plus latest;
  - macOS: Apple Clang, with the `gcc` alias cell removed;
  - Alpine/musl build + native tests;
  - FreeBSD build + native tests (Cirrus CI or a VM action);
  - Linux `-m32` native tests for size/conversion assumptions;
  - QEMU s390x or ppc64 for big-endian-sensitive compiled layouts;
  - `WITH_LISP={0,1}`, `-O0/-O2/-Os`, LTO, and debug/release assertions;
  - locale matrix (`C`, `C.UTF-8`, one non-English UTF-8 locale) and terminal
    matrix for a small PTY canary set (`xterm-256color`, `screen`, `tmux`).

  Print `CC --version`, target triple, `sizeof` facts, locale, and TERM. Run the
  full slow PTY suite only on the primary Linux lane; portability lanes can use
  native tests plus 5-10 PTY canaries.
- **Effort:** Medium.
- **CI cost:** Medium; most lanes are fast native builds and can be scheduled
  or path-filtered.
- **Signal:** Medium-high, especially for Fe's arena/pointer assumptions and
  tiny-regex-c's packed bytecode layout.

### Q10. Track quality signals and flakiness as first-class artifacts

- **Evidence:** `.ci/run-ci-steps.sh` already records per-stage states, timings,
  logs, and lane paths (`:105-120,310-333`), but results remain local files.
- **Recommendation:** emit one `quality.json` per run containing:
  - tool/compiler versions and configuration hash;
  - test/pass/skip/xfail counts and durations;
  - per-test PTY retry/failure history;
  - line/branch/function coverage;
  - complexity and mutation baselines;
  - analyzer findings by stable fingerprint;
  - fuzzer corpus/feature/exec/s/RSS/timeout metrics;
  - binary size and a few microbenchmark medians.

  Upload it and render a small trend dashboard. Nightly, shuffle PTY order and
  repeat each case 10 times under load; quarantine only with an owner, issue,
  expiry, and preserved failure logs. Continue treating XPASS as failure.
- **Effort:** Medium.
- **CI cost:** Low for collection; nightly flake burn-in is moderate.
- **Signal:** High over time; it turns “seems stable” into reviewable evidence.

## Phased implementation and ratchets

### Phase 0: connect and baseline (week 1)

1. Add the hosted `quality` job invoking the existing parallel runner.
2. Pin/print tool versions and upload all logs/artifacts.
3. Add standalone Fe and tiny-regex-c `check` to a new numbered stage.
4. Emit baseline JSON for coverage, complexity, test counts, durations, and
   analyzer findings. Do not fail on newly introduced advisory metrics yet.

**Exit criterion:** every current gate runs on every PR and a missing tool is a
hard failure, never a skip disguised as green.

### Phase 1: no-regression enforcement (weeks 2-3)

1. Enable root global/per-file coverage baselines at measured values and
   changed-line coverage in advisory mode.
2. Replace aggregate-only complexity gates with per-symbol no-increase
   manifests.
3. Expand warning/analyzer profiles, triage the baseline, and enable clean
   groups as errors.
4. Change fuzz smoke to short time budgets and run all kg/Fe/regex targets.

**Exit criterion:** new code cannot reduce measured coverage, increase a
function's complexity, add an unbaselined analyzer finding, or crash/timeout a
seeded sanitizer fuzzer.

### Phase 2: semantic strength (weeks 3-6)

1. Add search/replace, buffer/undo, Fe graph/unwind, and invariant-bearing regex
   fuzz targets.
2. Expand differential operation traces and exact compile-status comparison.
3. Add property/exhaustive-small-state tests.
4. Introduce changed-function Mull mutation testing; make mutation-score
   regressions blocking after two stable weeks.
5. Raise changed-line coverage to 85% blocking.

**Exit criterion:** the suite demonstrates that it detects wrong results, not
only that it executes lines without sanitizer findings.

### Phase 3: sustained depth and portability (ongoing)

1. Nightly 15-minute fuzz campaigns, weekly MSan/mutation/CodeQL/CBMC jobs.
2. Add musl, FreeBSD, 32-bit, big-endian, locale, and PTY canary lanes.
3. Trend quality JSON and enforce expiring suppressions/baselines.
4. Lower absolute debt ceilings only after refactors land; never raise them to
   accommodate routine feature growth.

## Suggested PR versus scheduled budget

| Gate | PR budget | Scheduled budget | Expected signal |
| --- | ---: | ---: | --- |
| Existing native + PTY tests | current | 10× shuffled PTY burn-in nightly | regressions, flakiness |
| ASan/UBSan/MSan/Valgrind | current parallel lanes | weekly optimized + alternate configs | memory/UB |
| All libFuzzer smoke | 5-10 s/target | 15 min/target nightly; MSan weekly | crashes, hangs, invariants |
| Regex differential | 2,000 fixed + rotating seed | 1-10M cases | semantic parity |
| Static analysis | changed/all TUs in parallel | CodeQL/full profiles weekly | latent defects |
| Coverage | one existing instrumented suite | none extra | regression ratchets |
| Mutation | changed pure functions, capped 10 min | full selected modules weekly | test effectiveness |
| Portability | primary + a few fast native lanes | full matrix weekly | platform assumptions |

## Honest ordering

Do not begin with more analyzers. First make the analyzers and fuzzers already
present unavoidable in hosted CI, then include the Fe/tiny surfaces that root
CI omits, then ratchet coverage and add semantic fuzz/property oracles.
Mutation testing is the best next ambitious investment after those foundations
because it will reveal where the large existing PTY and native suites are
assertion-weak. Portability expansion and dashboards are valuable, but they
should follow enforcement and semantic depth rather than distract from them.
