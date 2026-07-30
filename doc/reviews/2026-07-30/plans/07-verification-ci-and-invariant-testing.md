# Plan 07 — Hosted CI, semantic invariants, fuzzing, and quality tracking

## Goal

Turn the strong local verification suite into unavoidable hosted enforcement,
then improve its ability to detect wrong results—not merely crashes or executed
lines.

This plan covers: GitHub Actions running only `make`/`make check`; root CI
omitting the standalone Fe/Fex and tiny-regex-c suites; root coverage with no
threshold, no branch data and an unrepresentative file set; run-count fuzz
smoke over un-seeded corpora, and search/prompt code the keypress fuzzer never
links; first-match-only regex differential testing; aggregate complexity
ceilings with broken per-symbol identity; missing mutation/property/model
tests; and portability and quality-trend artifacts.

## Verification notes (checked against the tree, 2026-07-30)

Confirmed: `.github/workflows/build.yml` runs `make`, `make check`, a `DESTDIR`
install smoke and `make deb` over `{ubuntu,macos}-latest × {gcc,clang}`,
installing only `debhelper`/`build-essential`, and never invokes
`.ci/run-ci-steps.sh`. Nothing under `.ci/` builds `fe/` or
`fe/tiny-regex-c/`. Root `coverage` ends in a bare `lcov --summary` and
`COVERAGE_LCOV_ARGS` lacks `--branch-coverage` (`Makefile:155`), while both
subprojects enforce `COVERAGE_MIN_LINES ?= 80` with branch data
(`fe/Makefile:77,207`; `fe/tiny-regex-c/Makefile:98,111`). Fuzz smoke is
run-count only — `-runs=1000` for keypress, `-runs=50` for the other three,
with no `-max_len`, `-timeout`, `-rss_limit_mb` or `-artifact_prefix`
(`Makefile:229-259`).

Corrected:

- **"Incomparable-heavy differential" is wrong.** `make check-regex-differential`
  reports `cases=2000 diverged=0 incomparable=0` at the tracked seed, and the
  same at `REGEX_DIFF_SEED=12345 REGEX_DIFF_CASES=4000`. The gap is comparison
  breadth (first forward match only), not oracle noise.
- **The complexity ceilings are exact, not round aspirations.**
  `SCC_COMPLEXITY_MAX = 4208` and `src/` measures exactly 4208;
  `PMCCABE_FUNCTION_COMPLEXITY_MAX = 120` and the worst function measures
  exactly 120. Their defect is that they are *aggregate* (offsetting) and
  hand-maintained, not that they are loose.
- **The toolchain is not "named in `AGENTS.md`".** That file mirrors
  `CLAUDE.md` and lists no tool versions; the required set is derived from
  `.ci/*.sh` and Makefile tool variables (Phase 1).

## Principles

1. Connect existing gates before adding new tools.
2. A missing required tool is a failure, not a green skip.
3. Baseline current facts before imposing thresholds.
4. Ratchets prevent regression; they are not aspirational round numbers.
5. Semantic assertions and state models outrank more mutation count.
6. Slow/deep work belongs in scheduled lanes after fast PR enforcement exists.

## Phase 1 — Run the existing deep pipeline in hosted CI

### 1a. Prerequisite: make hosted `make check` honest

Three hard-coded local assumptions sit under `make check`:

- `utils/pty_accept.py:56` pins `EMACS = "/opt-3/emacs-31-lucid/bin/emacs"`.
  17 of the 224 cases in `test/pty/` use `oracle: emacs`; without that path the
  oracle spawn fails and the case is `ERROR` (`utils/pty_accept.py:623-624`),
  failing `make check`.
- 71 cases use `backend: tmux`, and `utils/pty_accept.py` imports `pexpect`
  and `yaml` at module scope. The workflow installs none of `tmux`, `emacs`,
  `pexpect`, `PyYAML`.
- `Makefile:220` invokes `python`, not `python3`.

So either the hosted job is currently red or it is not exercising the PTY layer
the way the local tree does. **Establish which from the last hosted run before
writing new workflow YAML.** Fix: add `--emacs`/`KG_PTY_EMACS` to
`utils/pty_accept.py` with a `shutil.which("emacs")` fallback (mirroring the
Makefile's `EMACS ?= emacs`); add `--require-tools`, used by CI, so a missing
`emacs`/`tmux` fails loudly there while a plain dev box still gets counted
`SKIP`s (principle 2 without breaking local work); install
`tmux emacs-nox python3-pexpect python3-yaml` in the workflow; use `python3`.

### 1b. Files

`.github/workflows/build.yml`, new `.github/workflows/quality.yml`,
`.ci/run-ci-steps.sh`, `.ci/ci-env.sh`, new `utils/print-tool-versions.sh`, and
a container/devcontainer definition if needed.

### 1c. Wiring

Prefer **one hosted job per numbered step** over a single `--parallel` job:

- the step list is a glob (`steps=(.ci/ci-[0-9][0-9]-*.sh)`), so a matrix over
  `.ci/ci-*.sh` needs no runner change and a new `ci-NN` joins both paths;
- `--parallel` gives every building step a throwaway worktree copy (only
  `ci-01-complexity` and `ci-07-format-check` run in-tree); eight copies plus
  eight `-B` rebuilds is the wrong shape for a 2–4 core hosted runner;
- `CI_PARALLEL_LANES` defaults to `nproc / 4` clamped to 2..6
  (`.ci/ci-env.sh:13-20`), so a hosted runner gets 2 lanes and `JOBS = nproc/2`;
- per-step jobs give per-step logs and re-runs for free, where `--parallel`
  buries them in `.ci/.run/logs/` and needs an upload step.

Keep `.ci/run-ci-steps.sh --parallel` as the local green-light path; add a
single hosted job running it end to end only if a full-machine runner exists.

Required tools, from `.ci/*.sh` and the Makefile, each version recorded by
`utils/print-tool-versions.sh`: `ccache`, `gcc` (`-fanalyzer`), `clang`
(asan/ubsan/msan), `valgrind`, `scc`, `pmccabe`, `lcov`/`genhtml`,
`clang-format`, `bear`, `clang-check`, `clang-tidy`, `cppcheck`, GNU
`parallel`, `rg` (used by `.ci/ci-06-static-analysis.sh`), IWYU, `emacs`,
`tmux`, `python3` + `pexpect` + `PyYAML`. Two are pinned to a local layout and
must be overridden in the image or `ci-env.sh`:
`IWYU ?= /opt-3/iwyu-21/bin/include-what-you-use` and `IWYU_TOOL`
(`Makefile:163-164`). Also record image digest, target triple and libc, CPU
count and lane settings.

Upload on failure: `.ci/.run/logs/`, the `.ci/.run/*.state` files, coverage
summaries, sanitizer/fuzzer artifacts, analyzer output.

Keep the simple build matrix as fast platform smoke, but drop the macOS `gcc`
cell or verify compiler identity: `/usr/bin/gcc` there is Apple Clang, so that
cell duplicates the `clang` cell.

### Acceptance

Every numbered stage runs on a test PR; deliberately breaking a
format/complexity/sanitizer test fails hosted quality; a missing executable
fails with a clear message (the pmccabe checker already reports empty stdin as
a broken gate, `utils/check_pmccabe_complexity.py:44-52`);
`make check-regex-differential` does not silently self-skip in CI; and runner
lock/copy behavior stays intact — both modes lock `.ci/.run`, and a reused
self-hosted workspace can present a stale lock, which the runner reports and
takes over.

## Phase 2 — Add standalone subproject CI to the root runner

### Files

New `.ci/ci-11-subprojects.sh` (joins the glob automatically; the glob is two
digits, so numbering ends at 99), `.ci/ci-env.sh`, and the Fe/tiny Makefiles
only if build isolation needs knobs.

### Changes

Both subprojects already ship numbered runners (`fe/.ci/run-ci-steps.sh`,
`fe/tiny-regex-c/.ci/run-ci-steps.sh`), so delegate rather than re-list
targets, with an explicit-target fallback for the PR subset:

```sh
make -C fe check complexity-check pmccabe-check format-check
make -C fe/tiny-regex-c check complexity-check pmccabe-check format-check
```

All of those exist; tiny additionally has `verify` (CBMC) and the Python
drivers `test-pyok`/`test-pynok`.

Object-file collisions are a smaller risk than the earlier draft assumed: root
compiles `fe/fe.c` to `src/fe.o` and `fe/tiny-regex-c/re.c` to
`src/tiny_regex.o` (`Makefile:182-193`), so subproject `*.o` files are never
consumed by the root build. The real hazards are subproject `coverage-clean`
deleting `fe/*.gcda` under a running root coverage lane, and `fe/.ci` carrying
two `ci-06-*` scripts (fuzz-smoke and static-analysis), which is legal for the
glob but makes step numbering ambiguous in reports. Under `--parallel` this new
stage builds, so it must **not** be listed in `in_tree_steps`; it then gets its
own worktree copy and both hazards disappear.

CBMC/formal checks may start scheduled until runtime is measured.

### Acceptance

Introduce a deliberate failure in a Fex test and in a tiny standalone test and
prove the root runner catches both.

## Phase 3 — Emit one machine-readable quality artifact

### Files

New `utils/quality_report.py`; `utils/pty_accept.py` (`--json`); `Makefile`
(`check-unit` result emission); `.ci/run-ci-steps.sh`; `.gitignore`.

### Changes

Two producers already exist and should be read, not scraped:

- `.ci/.run/<step>.state` already carries
  `step/state/start/end/elapsed/exit/log/lane` (`state_set()` in
  `.ci/run-ci-steps.sh`) — the source for per-stage status and duration;
- `utils/pty_accept.py` already accumulates `PASS/SKIP/FAIL/XFAIL/XPASS/ERROR`
  counts (`:689`); add `--json <path>` emitting per-case status, duration,
  backend, oracle and retry count.

`check-unit` is a shell loop that hardcodes `# SKIP: 0` and `# XFAIL: 0`
(`Makefile:196-218`): the native layer has no skip/xfail concept, so either
give it one or record the honest zeros rather than implying parity with PTY.

`quality.json` should contain: commit/submodule SHAs; tool/config versions;
per-stage status and duration; native/PTY pass/skip/xfail counts with per-case
duration; line/function/branch coverage by component and file; per-file and
per-function complexity; analyzer findings by stable fingerprint; fuzzer
corpus/features/exec-per-second/RSS/timeouts; binary sizes; Plan 08 benchmark
medians/p95 where present. Validate the schema in CI; upload but never commit.

## Phase 4 — Establish coverage ratchets

### Files

`Makefile`, `.ci/ci-02-coverage.sh`, checked-in `.ci/coverage-baseline.json`.

### Baseline first — the current artifact is not usable

The checked-out `coverage/src.info` does report 55.3% lines (3131/5665) and
68.2% functions (174/255), but over **20 source files while `SRCS` lists 28**:
`help.c`, `lisp.c`, `keybind.c`, `mode.c`, `localvars.c`, `compile.c`,
`width.c` and `dired.c` are absent from the tracefile entirely. A percentage
whose denominator omits eight shipped files cannot become a ratchet.
Regenerate on the hosted image and confirm the file list equals `SRCS` before
recording any baseline.

### Changes

Collect line, function and branch coverage; report kg, Fe core/Fex and
tiny-regex separately plus a combined shipped-code view; keep per-file results.

Enforcement: (1) no regression below the exact per-file/global baseline;
(2) improvements automatically propose lifting it; (3) changed-line coverage
advisory first; (4) after two stable weeks, a reviewed changed-line threshold;
(5) exclusions only for generated tables or unreachable platform shims, with
comments.

### Pitfall: parallel gcda merging

`make coverage` builds `src/kg` with `--coverage` and then runs the full
`check`, so the instrumented binary *is* the one the PTY cases execute — that
concern from the earlier draft is settled. But the PTY layer runs `PTY_JOBS`
cases at once (8 in CI), all the same instrumented binary writing the same
`src/*.gcda`; `COVERAGE_LCOV_ARGS` already carries
`--ignore-errors inconsistent,gcov` (`Makefile:155`), a standing admission that
those merges are lossy. Before enforcing per-file numbers, either run the
coverage lane with `PTY_JOBS=1` or give each case its own
`GCOV_PREFIX`/`GCOV_PREFIX_STRIP` (`utils/pty_accept.py` already assembles a
per-case environment for `HOME`) and merge afterwards. Record the choice in
`quality.json`; a ratchet on noisy inputs gets reverted.

## Phase 5 — Replace aggregate complexity with per-symbol ratchets

### Files

`utils/check_scc_complexity.py`, `utils/check_pmccabe_complexity.py`, a new
baseline manifest, Makefile metric variables.

### Current facts

`scc` total for `src/` is 4208 against a 4208 limit: zero headroom, but
offsetting is possible (a `+12` in `src/kbd.c` passes if another file loses
12). Worst file is `src/bufmgr.c` at 461 against 520. Worst function measures
120 against 120. Both checkers already fail closed when their tool is missing
(empty stdin → explicit failure; unparsable JSON → exit 2), so principle 2
already holds here.

### Changes

Track per file and function: cyclomatic complexity, function LOC/statements,
file complexity/LOC, optionally nesting/cognitive complexity from a stable
second tool. Rules: no increase for an existing symbol without a reviewed,
expiring exception; new functions target ≤15; functions above 40 must not
increase and should be split when substantively modified; decreases are
accepted automatically; renames need explicit baseline mapping or count as
remove/add. Keep total/per-file ceilings as a transition backstop.

### Fix the symbol-identity bug concretely

The 120-complexity entry is reported as `src/kbd.c(506): __attribute__`.
`pmccabe` does not preprocess, so the GCC-only
`__attribute__((optimize("O0")))` at `src/kbd.c:505` is taken as the function
name; the real symbol is `editor_process_keypress()` at `src/kbd.c:507`. A
per-symbol baseline keyed on that string would be silently invalidated by any
edit to the pragma block. Minimal fix in `utils/check_pmccabe_complexity.py`:
when the extracted name is not a plain identifier (or is a known
attribute/pragma token), resolve it by reading the cited `file(line)` and
taking the first identifier followed by `(`. Otherwise key the baseline on
`file:normalized-name` from a parser-aware tool. Never accept `__attribute__`
as a symbol name.

## Phase 6 — Convert fuzz smoke to time budgets

### Files

`Makefile`, `fe/Makefile`, `fe/tiny-regex-c/Makefile`,
`.ci/ci-09-fuzz-smoke.sh`, `test/fuzz-seeds/`, fuzz docs.

### Changes

The subprojects already parameterize what the root hardcodes: `FUZZ_RUNS`,
`FUZZ_MAX_LEN`, `FUZZ_TIMEOUT`, `FUZZ_RSS_LIMIT_MB`, `FUZZ_VERBOSITY`,
`-artifact_prefix` and a `-dict` (`fe/Makefile:114-131`,
`fe/tiny-regex-c/Makefile:183-190`). Adopt those names at the root and add
`FUZZ_MAX_TOTAL_TIME` rather than inventing a second scheme.

PR smoke:

- `-max_total_time=5` (or 10) per target instead of `-runs=N`;
- tracked seeds **for every target**: only `test/fuzz-seeds/regex` exists (15
  inputs), so `fuzz-keypress-smoke`, `fuzz-dirlocals-smoke` and
  `fuzz-localvars-smoke` start from an empty corpus on a fresh checkout and
  `-runs=50` explores almost nothing. Add
  `test/fuzz-seeds/{keypress,dirlocals,localvars}` plus `fuzz-*-seed` targets
  modelled on `fuzz-regex-seed` (`Makefile:243-245`);
- bounded input length, RSS and per-input timeout;
- `-artifact_prefix=$(TESTDIR)/fuzz-artifacts/<target>/`, where `.gitignore`
  already expects crashes — no root target writes there today, so crash inputs
  currently land in the working directory;
- deterministic options logged.

Scheduled: 15 minutes per ASan/UBSan target nightly; MSan weekly; corpus
merge/minimize; commit only small semantic/crash seeds.

Cost check: `.ci/ci-09-fuzz-smoke.sh` does `make clean` then `make fuzz-smoke`
and owns a worktree copy under `--parallel`, so four 10 s budgets add ~40 s to
a lane that is not the critical path (five of the ten steps are ~99% PTY).

Track feature/edge coverage, corpus growth, exec/s, peak RSS and timeouts. Fail
crashes/timeouts; report coverage drift until stable.

## Phase 7 — Add stateful semantic fuzz/model targets

### kg edit/undo

New `test/fuzz_edit_model.c`: compare kg rows to a flat reference string over
insert/delete/newline/kill/yank/undo/save; assert row NUL termination,
cursor/mark boundaries, exact undo and dirty truth.

### buffer/window

New native `test/test_winmgr.c` and an optional stateful fuzzer:
create/switch/split/cycle/delete/resize buffers and windows; assert active
counts, ownership, current mapping, per-view position, shared buffer
text/undo, no stale pointers. Wiring: add the binary to `TESTBINS` plus an
`EXTRA_winmgr := ...` line for the secondary-expansion link rule
(`Makefile:330-354`). `src/winmgr.o` is linked into no test binary today, so
pick a stub set (`test/stubs.c`, `stubs_noyank.c`, `stubs_buffer.c`,
`stubs_extra.c`) deliberately and expect `bufmgr.o` to come with it.

### minibuffer/prefix

`test/test_minibuf.c` already exists and covers history ordering/eviction,
multibyte backspace, overflow retirement and display width (20 cases). Extend
it instead of starting a new harness, keeping the split CLAUDE.md requires:
pure state functions natively, the `editor_read_line()` loop in PTY cases
because it depends on terminal input. Add UTF-8 edit/draft/cancel/EOF paths and
prefix `{supplied,value}` propagation through direct/C-x/C-c/M-x/macro
dispatch.

### search/replace

Do not stub it out — with the accurate starting point: `src/search.c` is not
compiled into the keypress fuzzer at all (`FUZZ_SRCS`, `Makefile:101-106`) and
`test/fuzz_stubs.c:195` supplies a no-op `editor_query_replace()`. Linking
`search.c` requires replacing the canned prompt stubs (`editor_read_line*`
return `MINIBUF_CANCELLED`, `test/fuzz_stubs.c:77-99`) with scripted answers
drawn from the fuzz input. Then assert that search/next/skip/replace/all/cancel
reach their states, that hitting an iteration cap is a failure rather than a
stop condition, and that text/undo/progress invariants hold exactly.

### Fe and regex

Fe: tiny-arena forced GC; rooted/released object graphs; native re-entry and
errors at allocation positions; cyclic write/mark/equality; external cleanup
exactly once. Regex: span bounds/order; normalized offsets; forward/backward
agreement; empty-match progress; engine/wrapper status consistency.

Dropped from the earlier draft: "concurrent/reentrant execution". kg is
single-threaded and `fe/tiny-regex-c/re.c` holds no mutable globals (only
`const` tables at `re.c:1103,1119`). Re-verify only after Plan 06 introduces an
execution context, and then phrase it as *nested* use — a Lisp callback that
searches while a search is in progress.

Prefer exhaustive small-state enumeration to random generation where the state
space is tractable.

## Phase 8 — Strengthen differential testing

### Files

`utils/regex_differential.py`, `utils/regex_oracle.el`,
`test/regex_differential.c`; a Fe/Emacs pure-subset driver later.

### Regex

Measured starting point: 0 divergences and 0 incomparable cases at 2000 and
4000 cases — the harness is not noisy, it is narrow.
`test/regex_differential.c:43-72` asks the engine once and prints the single
first forward match from offset 0; `kg_regex_match_backward()`
(`src/regex.h:34`) is never exercised.

Extend the wire protocol with a mode column (forward-all / backward / icase /
replace) rather than adding a second binary, keeping the byte-offset conversion
in `utils/regex_oracle.el` intact. Compare compile acceptance/status; every
successive forward match; backward matches; byte/glyph offset policy; ICASE;
captures; zero-width matches; replacement expansion/iteration.

Separate two populations: exact supported-subset conformance (today's grammar)
and broader dialect characterization with a reason-coded allowlist. The
exclusions are already documented as comments in `utils/regex_differential.py`
(ASCII-only classes, `\?`, quantified quantifiers, groups past
`MAX_GROUPS = 9`); make each a machine-readable entry with a reason code so the
characterization lane can enable one at a time and record its expected
divergence count.

PR: fixed 2,000 plus a rotating deterministic seed, recorded in `quality.json`.
Scheduled: 1–10 million.

### Fe/Emacs-shaped subset

After Fe fixes: normalize reader/printer values; compare arity, `let`, macro
expansion, condition names and pure string/list functions; exclude known
language differences explicitly.

## Phase 9 — Mutation testing

Pilot Mull with Clang on fast pure modules: regex, `src/localvars.c`,
`src/width.c`, `src/buffer.c`/`src/undo.c`, Fe core. Create per-module
baselines: changed functions on PR, capped; full selected modules weekly; fail
on score regression rather than an arbitrary 100%; publish surviving mutants
with file/line/operator; turn every valuable survivor into a focused test. If
Mull integrates poorly with C23 or the pinned tool versions, evaluate an
alternative against one module before committing project-wide.

## Phase 10 — Portability matrix

Add in order:

1. verified minimum/latest GCC and Clang on Linux; 2. Apple Clang on macOS;
3. musl/Alpine native tests; 4. `WITH_LISP=0` beyond the primary compiler
   (`.ci/ci-08-with-lisp-0.sh` already covers the default toolchain, so this
   row is about other lanes, not a new gate); 5. `-O2` and LTO smoke — the
   default build is already `-Os` (`Makefile:7`) and the sanitizer lanes are
   `-O0`, so those two are the untested points; 6. 32-bit
   conversion/alignment tests; 7. FreeBSD native tests; 8. big-endian QEMU for
   Fe/object/regex representation; 9. locale and `TERM` canaries;
10. small-thread-stack regex/Fe recursion tests.

Run the full PTY suite only on primary Linux; other lanes get native tests plus
a small PTY canary set, and must not inherit a `PTY_TIMEOUT` tuned for a fast
box — `.ci/ci-env.sh` already doubles it to 40 s under `--parallel` because
several sanitizer lanes driving PTYs at once manufacture timeouts.

## Phase 11 — Flake and trend management

Nightly: shuffle PTY order — cases are sorted today (`PTY_TESTS = $(sort ...)`,
`Makefile:166`) and dispatched to a process pool, so add `--shuffle`/`--seed`
to `utils/pty_accept.py` and record the seed; repeat timing-sensitive cases 10×
under load; record duration and failure distribution; never silently increase
sleeps or timeouts, and never take `key_delay` below 0.05, which is a kg
semantic boundary (30 ms paste detection), not a performance knob; quarantine
only with owner, issue, expiry and preserved logs; keep XPASS failing
(`utils/pty_accept.py:744` already does).

Render `quality.json` trends. Start advisory; enforce only stable signals.

## Commit sequence

1. Hosted `make check` prerequisites (Phase 1a) — smallest change, unblocks
   the rest. 2. Hosted deep job. 3. Subproject stage. 4. Quality JSON.
5. Coverage baseline, after regenerating a full-file tracefile.
6. Complexity baseline, after the `__attribute__` fix. 7. Time-budget fuzz plus
   per-target seeds. 8. Stateful models. 9. Differential traces.
10. Mutation pilot. 11. Portability lanes and trends.

## Acceptance

The program is successful when a PR cannot: bypass an existing local gate;
lower measured coverage; increase a function's debt unnoticed; add an analyzer
finding; crash or time out a seeded fuzzer; violate edit/search/GC/regex state
invariants; or change supported regex/Fe semantics without a minimized
differential case.
