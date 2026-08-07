# kg agent notes

kg is a small Emacs-style terminal editor written in C23. Read `README.md` first for project goals and user-facing behavior.

## Repo shape
- `src/`: editor implementation
- `test/`: standalone regression tests and test stubs
- `doc/`: man page, screenshots, roadmap
- `Makefile`: build, test, install, release targets

## Build and test
- Build: `make` or `CC="ccache cc" make`
- Run tests: `make check` (e.g. `make check 2>&1 | grep -E "^(FAIL|ERROR|XPASS)|# (FAIL|ERROR|TOTAL|PASS|SKIP|XFAIL|XPASS)" | head -30; echo "CHECK EXIT=${PIPESTATUS[0]}"`). Also run `make complexity-check` before and after larger chunks of work.
- Clean binaries/objects: `make clean` or `make distclean`
- `make check` now runs two layers:
  - native unit tests in `test/test_*.c`
  - PTY-backed acceptance cases from `test/pty/*.yaml` via `utils/pty_accept.py`
- Run a single PTY case (useful while iterating) with:
  `python3 utils/pty_accept.py --kg src/kg <case.yaml>` (add
  `--timeout`, `--startup-delay-add`, `--key-delay-add` for flaky cases). A
  failing case prints a unified diff of the expected vs actual saved file.
  The harness needs `pexpect` and `PyYAML`; if `python3` lacks them, use
  the interpreter that has them (the Makefile picks one automatically and
  it can be overridden with `make PYTHON=...`).
- The suite skips rather than fakes what it cannot run: a case needing
  `tmux`, or the `emacs` oracle, SKIPs with a printed reason and count when
  the tool is missing. `--require-tools` (hosted CI passes it via
  `make check PTY_ACCEPT_ARGS=--require-tools`) turns that into an upfront
  failure naming the tool. The oracle binary is `--emacs`, else
  `$KG_PTY_EMACS` (`make check KG_PTY_EMACS=...`), else `emacs` on PATH,
  else the `/opt-3` developer-box pin.
- PTY cases run concurrently; `--jobs` (Makefile `PTY_JOBS`, CI default 8)
  sets how many. Use `--jobs 1` when debugging a case so its output is not
  interleaved with other work on the box.
- `make check-regex-differential` compares kg's regex engine against
  Emacs' own matcher on randomly generated patterns and subjects
  (`utils/regex_differential.py` drives `test/regex_differential.c` and
  `utils/regex_oracle.el`). Seeded, so a failure reproduces: it prints the
  seed, and the offending pattern, subject and spans. `REGEX_DIFF_CASES`
  (default 2000, ~0.2 s) and `REGEX_DIFF_SEED` are the knobs for hunting;
  the target skips itself with a message when `emacs` is not on PATH. It
  is not part of `make check`; CI runs it as `.ci/ci-10-*.sh`. Both sides
  report BYTE offsets — Emacs reports character offsets natively and the
  oracle converts, so do not remove that conversion.
  Every generated pattern is asked in two modes (`--modes f,fa`): the
  first match from offset 0, and every successive match iterated by
  `kg_regex_next_offset()`'s rule, which is where empty-match progress
  and leftover capture registers show up. Backward matching is still
  uncompared; `kg_regex_match_backward()`'s selection rule (last match
  ending at or before the limit, taking the plain forward match at each
  candidate start) is not Emacs' bounded backward search, so an oracle
  for it has to encode kg's policy rather than read Emacs'.
- Fuzz smoke runs are a time budget, not a run count: `FUZZ_MAX_TOTAL_TIME`
  (5 s per target) with `FUZZ_MAX_LEN`, `FUZZ_TIMEOUT`,
  `FUZZ_RSS_LIMIT_MB` and `FUZZ_VERBOSITY`, the same names both
  subprojects use, and `-print_final_stats` so execs/s and peak RSS are
  in the log. Every target has tracked seeds under `test/fuzz-seeds/<target>`
  and a `make fuzz-<target>-seed` that copies them into the gitignored
  working corpus; `make fuzz-seed` does all five. Crash, timeout and OOM
  inputs land in `test/fuzz-artifacts/<target>/`.
- `make fuzz-regex-seed-replay` runs every tracked regex seed once without
  mutation. Each harness documents its input encoding in its header
  comment (`test/fuzz_regex.c`, `test/fuzz_keypress.c`, ...).
- `make coverage` ends in a ratchet, not a printed number:
  `.ci/coverage-baseline.json` holds every `src/*.c` file's line and
  function rate, `make coverage-check` fails when one falls below its
  floor, and `make coverage-baseline` is the one command that rewrites
  the file (bank an improvement; a drop needs a reason in the commit).
  Branch data is collected and reported but not yet a floor. The lane
  keeps `PTY_JOBS=8`: three runs (two at 8, one at 1) agreed file for
  file, so the parallel gcda merge is not lossy here and the 5m37
  serial run buys nothing over 1m02. A file may lose up to 4 covered
  lines (the tree, 8) before the gate fires: repeated runs of one commit
  wobble by up to 3 lines in `src/syntax.c`, whose highlighting paths
  depend on what the PTY cases painted. Function coverage was identical
  in every run measured and gets no slack.
- `make gateway-check` (part of `.ci/ci-01-*.sh`) is a census, not a ban:
  `.ci/mutation-gateway.json` records, per `src/*.c`, how many raw row
  primitives it calls, how many undo records it writes by hand, how many
  times it touches `suppress_undo`, and how many times it writes a row's
  text fields directly. No count may rise and no unlisted file may
  appear; `make gateway-baseline` banks a decrease. The manifest is
  follow-up Plan 02's remaining migration work written down.
- Every `make check` writes machine-readable results to `test/.results/`
  (gitignored): `unit.json` from `utils/run_unit_tests.py`, `pty.json`
  from `utils/pty_accept.py --json`, both with per-case status and wall
  time. `--parallel` keeps each lane's copy under
  `.ci/.run/results/<step>` and ends by writing `.ci/.run/quality.json`
  (`utils/quality_report.py`): stages and durations, both layers' counts
  and slowest cases, coverage against its floor, the complexity manifest,
  the pins. `utils/print-tool-versions.sh` prints the toolchain and the
  box; hosted CI runs it before every step.
- Performance evidence is counters first, wall clock second.
  `src/perf.h` declares counters that compile to nothing unless
  `KG_PERF_COUNTERS` is 1 (the KG_SHOW_TILDE/KG_FUZZ pattern), so the
  shipped editor carries none of it. The counting build lives in its own
  object directory, `test/perfobj/`, and never mixes with `src/*.o`:
  `test/test_perf` is that build minus `main.o` plus the test harness,
  and `test/perfobj/kg` is the same objects plus `main.o`. A counting kg
  writes every counter as JSON to `$KG_PERF_OUT` when it exits.
- `test/test_perf.c` is the gate. It runs inside every `make check`, and
  it asserts shapes -- reallocations against `c*log2(rows)`, exactly one
  `editor_update_row()` per logical replacement -- because a counter is
  the same number on a loaded box, in a sanitizer lane and under
  valgrind. Each case names the performance property whose evidence it is.
- `make bench` (`utils/bench.py`, JSON to `test/.results/bench.json`)
  drives `test/perfobj/kg` over generated corpora in a real pty and
  reports median/p95 wall time, peak RSS and the counters per case.
  It is deliberately *not* a CI step: benchmark numbers taken while five
  sanitizer lanes drive PTYs measure the box, not the change, and a gate
  that flakes gets switched off. Its times are comparable only against
  another counting build -- never against a release `-Os` one. The
  `startup` case is the constant to subtract by eye. `--big` adds the
  1M-line corpus; corpora are cached in the gitignored `test/.bench/`.
- Hosted CI is two workflows: `build.yml` is platform smoke, and
  `quality.yml` runs one job per `.ci/ci-NN-*.sh`, discovered from the
  same glob, with `--require-tools` so a missing tmux or Emacs fails
  instead of skipping.
- The toolchain, and who needs it.  Every one of these is a bare name the
  Makefile or a `.ci` step resolves through `PATH`, overridable by the
  variable in parentheses; nothing is pinned to an absolute path any more.
  `utils/print-tool-versions.sh` prints which of them this box has, and
  what version, and is the thing to run first when a step fails only here.
  - `make`, a C23 compiler (`CC`, default `gcc`) and `clang` (`CLANG_CC`,
    `FUZZ_CC`) -- sanitizer lanes `ci-03`..`ci-05` and the fuzz targets are
    clang-only.  `ccache` is optional and only ever a speed-up.
  - `python3` or `python` (`PYTHON`) with `pexpect` and `PyYAML`: the PTY
    harness and every ratchet script under `utils/`.  The Makefile picks
    the first interpreter that can import both.
  - `tmux`: PTY cases with `backend: tmux`.  `emacs` (`EMACS`,
    `KG_PTY_EMACS`): `oracle: emacs` cases and
    `make check-regex-differential`.  Both SKIP with a reason when
    missing, unless `--require-tools` is passed.
  - `scc` (`SCC`, tested at v3.7.0) and `pmccabe` (`PMCCABE`): the
    complexity ratchets in `ci-01`.
  - `lcov`, `genhtml`, `gcov`: `make coverage`, `ci-02`.
  - `clang-format` (`CLANG_FORMAT`): `ci-07`.
  - `bear` (`BEAR`), `clang-check`, `clang-tidy`, `cppcheck`,
    `include-what-you-use` and `iwyu_tool.py` (`IWYU`, `IWYU_TOOL`), and
    GNU `parallel` (`GNU_PARALLEL`): `ci-06`.  `parallel` must be GNU
    parallel, not moreutils'.
  - `valgrind` (`VALGRIND`): `ci-03`.
  - `cbmc` (`CBMC`): `fe/tiny-regex-c`'s `make verify` only.  Nothing kg
    runs needs it -- `make check` there runs `verify-syntax`, which asks
    an ordinary compiler whether the CPROVER harness still builds.
  - Hosted CI additionally needs `jq` (step discovery) and `go` (to
    install `scc`); see `.github/workflows/quality.yml`.
- To iterate on one CI gate, run its script directly, e.g.
  `.ci/ci-01-*.sh`; shared defaults come from `.ci/ci-env.sh`.
- `CC` and `CFLAGS` are environment-overridable, e.g. `CC="ccache clang" CFLAGS="..." make`.
- Final green light comes from running `.ci/run-ci-steps.sh` (static
  analysis, sanitizers, compilation warnings as errors...). The runner
  dispatches numbered scripts `.ci/ci-01-*.sh` through `.ci/ci-12-*.sh`;
  run one directly when iterating on a specific phase. The step list is a
  glob, so a new `.ci/ci-NN-*.sh` joins the run with no runner change.
- `.ci/ci-12-subprojects.sh` runs the submodules' own fast suites from the
  root: `make -C fe check complexity-check pmccabe-check format-check` and
  the same for `fe/tiny-regex-c` (~12 s together). kg links `fe/fe.c` and
  `fe/tiny-regex-c/re.c`, so their standalone behaviour -- Fex, the Fe
  script suite, the regex test vectors -- is kg's too. Their numbered
  runners (`fe/.ci`, `fe/tiny-regex-c/.ci`: valgrind, MSan, coverage,
  clang-analyzer) are minutes each and stay the submodule's own green
  light before a pin moves; this stage is not a replacement for them.
- CI parallelism is controlled with `JOBS` (default: `nproc`), e.g.
  `JOBS=8 .ci/run-ci-steps.sh`. Shared CI defaults live in
  `.ci/ci-env.sh`.
- `.ci/run-ci-steps.sh --parallel` runs the steps concurrently. Every step
  that builds gets a throwaway copy of the working tree (uncommitted
  changes included, build artifacts left behind), because the steps
  otherwise fight over `src/*.o` and `make clean`. Each step writes its own
  log under `.ci/.run/logs/`; the terminal only gets a PASS/FAIL summary
  and a replay of the failing logs, and the exit status is non-zero when
  any step failed. `CI_PARALLEL_LANES` caps concurrency, defaulting to
  `nproc / 4` clamped to 2..6; each lane then builds with `nproc / lanes`
  jobs and the PTY suite gets a longer timeout, since several sanitizer
  lanes driving PTYs at once otherwise manufacture flaky timeouts. Six
  lanes is enough to start every slow step at once: five of the ten steps
  are ~99% PTY suite, which waits on terminal timing rather than cores.
- `.ci/run-ci-steps.sh --status` reports what the runner in this tree is
  doing, or last did: per-step `PENDING`/`RUNNING`/`PASS`/`FAIL` with
  timings and log paths. It never blocks and starts nothing, so poll that
  instead of guessing from the process table.
- Both run modes take a lock in `.ci/.run`; a second run in the same tree
  is refused (it would corrupt the first one's objects), and a lock left
  behind by a process that is gone is reported as stale and taken over.

## Editing expectations
- Keep changes small and local; this codebase values minimalism.
- Match existing C style: C23, tabs, short helper functions, Linux-kernel-like brace/layout conventions.
- Avoid new dependencies unless explicitly requested.
- A new module gets its own header. `src/def.h` is the core: the key
  enum, `erow`, the buffer/window/session structs and the globals every
  module reaches for. Everything a *single* module owns -- its types, its
  constants, its function declarations -- goes in `src/<module>.h` with an
  include guard, in the shape `src/localvars.h` already has: self-contained
  (it includes or forward-declares what it needs and nothing more), so it
  compiles on its own. `def.h` includes it only when `def.h`'s own
  definitions need the type; consumers include it directly, which is what
  IWYU (`make iwyu`, `.ci/ci-06`) will make them do anyway. `make
  header-check` compiles every `src/*.h` standalone and is part of
  `make check`. Do not add a module's declarations to `def.h` -- that is
  how `def.h` got to 1279 lines, and the per-file scc cap is 520.
- When behavior changes, add or update a focused test under `test/`.
- Prefer the existing C harness for pure logic and `test/pty/*.yaml` for interactive editor behavior.
- If you change user-visible behavior or keybindings, update `README.md`
  and/or `doc/kg.1`; also update the built-in help table in `src/help.c`
  when a keybinding changes.  `make docs-check` (part of `make check`,
  `utils/check_help_drift.py`) is the dumb half of that: every key the
  help table names must be spelled somewhere in `doc/kg.1`, with the
  handful of "the man page writes it in roff" spellings listed in the
  script rather than tolerated.  It does not check the other direction --
  kg(1) documents far more than one screen of help can.
  Generating the help table's *text* from `struct named_cmd::summary`
  would make this structural rather than checked, and is the right end
  state, but it needs two things that do not exist yet: built-in keys
  resolving to command names (follow-up keymap Plan 01), and a second,
  ~15-column
  summary per command, since the table's cells are that wide and the
  registry's summaries are up to 60.
- Command policy is one table: `cmdtable` in `src/cmd.c`, whose
  `struct named_cmd` (name, handler, flags, one-line summary) lives in
  `src/cmd.h`.  `CMD_EDITS_BUFFER` is the read-only verdict and
  `CMD_LISP_CALLABLE` is what `(command-execute ...)` may reach; both are
  read only by `cmd_invoke()`, the single route into a command.  Add a
  command by adding a row, and keep `test/test_cmd.c` green -- it asserts
  the table is sorted and unique and that every entry has a handler and a
  summary of at most 60 columns.  `src/kbd.c`'s `readonly_blocked_keys[]`
  is the one remaining second opinion; it goes when built-in keys resolve
  to command names.
- Character classification: prefer `ascii_is_print/digit/space()` from
  `def.h` wherever the grammar is ASCII by definition (syntax scanning,
  local variables, key codes). Where libc `<ctype.h>` genuinely belongs,
  pass `(unsigned char)` -- it is undefined for any other negative value,
  and kg never calls `setlocale()`, so in the `"C"` locale it says
  nothing useful about a byte >= 0x80. `.ci/ci-11-*.sh` runs the unit
  suite under both `-fsigned-char` and `-funsigned-char`.
- Nothing kg reads may become terminal syntax. Untrusted bytes reach the
  screen only through `display_glyph_at()` (`src/width.c`), which also
  decides their width; styling is the renderer's, never spelled into a
  string a caller interpolates into.
- A row is addressed in three coordinate spaces -- bytes of `row->chars`,
  bytes of `row->render` (which is how `row->hl` is indexed too), and
  display columns. `doc/coordinates.md` is the table of which function
  produces and consumes which, and the naming rule that keeps them
  apart; read it before adding one, and state the space at a new seam
  with `KG_ASSERT_CHARS_OFF`/`KG_ASSERT_RENDER_OFF` (armed by
  `-DKG_DEBUG_COORDS=1`, which `.ci/ci-04` builds with).

## Useful context
- `doc/TODO.md` tracks planned editor features and known technical debt.
- Tests are simple native binaries; prefer extending that harness before inventing new infrastructure.
- Some native tests link editor stubs rather than the full editor. If the
  behavior depends on real cursor movement, terminal input, windows, or
  saved-file output, use a PTY case instead of a C unit test.
- PTY acceptance cases operate on saved-file outcomes. They support either:
  - `expected_saved`: compare kg output to an explicit saved-file result
  - `expected_saved_any`: compare kg output to any one of several acceptable saved-file results
  - `oracle: emacs`: compare kg against `emacs -q -nw`
- Always set `filename` deliberately in PTY cases; editor mode and syntax behavior depend on the extension.
- PTY cases can choose `backend: pexpect` or `backend: tmux`; use `oracle_backend` when the Emacs oracle needs a different driver than kg.
- `startup_delay` and `key_delay` exist for timing-sensitive interactive cases; keep them explicit and case-local.
- kg's runs no longer sleep `startup_delay`: the harness polls until kg has
  painted its first frame and gone quiet, so a plain build waits ~3 ms and
  the same binary under valgrind waits ~0.33 s. `startup_delay` is now the
  fallback deadline, and the sleep the Emacs oracle still takes.
- `key_delay` is not "time for kg to keep up" -- keys queue in the pty. It
  is bounded below by kg semantics: under 30 ms between keys kg decides it
  is seeing a paste (`editor.paste_mode`) and drops auto-indent and
  autocompletion, and a bare `ESC` merges with the next key unless they are
  more than 100 ms apart. Do not take the default below 0.05.
- A tmux case ends when the pane has been *unchanged* for 50 ms, which
  cannot be told apart from "kg has not painted yet" -- a kg still running
  a compilation, a shell command or an arena-filling hook is silent. The
  fix is `--settle-floor` (`PTY_SETTLE_FLOOR`, set by `.ci/ci-env.sh`: 0.7
  serial, 1.5 under `--parallel`, unset for a plain `make check`): the
  minimum a tmux case waits after its last key before the pane may be
  called settled. It is paid once per tmux case rather than once per key,
  which is what makes it cheaper than the `key_delay` such a case would
  otherwise need. Size a case's own cover against the slowest lane *under
  load* -- plain, valgrind and MSan latencies here span two orders of
  magnitude, and a case only ever fails in the lane nobody measured.
- `dimensions: [rows, cols]` is available for viewport-sensitive cases.
- tmux-backed cases can assert visible screen content with `expected_screen_contains` and `expected_screen_not_contains`.
- Known discrepancies can be checked in as `xfail: true`; `XPASS` fails `make check` so expectations get cleaned up once behavior changes.
- Key tokens in PTY YAML are literal unless named. Use `SPC` for an
  actual space key, `RET` for Enter, `M-RET` for Meta-Enter (sent as
  one ESC+CR token so the pair lands inside kg's escape window),
  `C-?` for Backspace, and `C-q` followed by the next token for quoted
  input. `Home`, `End`, `C-Home`, `C-End`, `S-Home`, `S-End`, `Up`,
  and `Down` are named tokens (sent as xterm tilde / modified tilde /
  cursor sequences).
  PageUp/PageDown have no named tokens; emit their escape bytes via
  `M-[` plus the letter/digit/`~` (e.g. `M-[`, `H` for Home on
  terminals that send `ESC[H`).
  `BYTE=e2` sends one raw byte named in hex. Every other token is UTF-8
  encoded on the way out, so this is the only way to send a byte that is
  not valid UTF-8; it needs `backend: pexpect`.
- When using `oracle: emacs` outside `make check`, set a real terminal,
  e.g. `TERM=xterm-256color`, or Emacs may refuse to start under
  `TERM=dumb`.
- Emacs may save a final newline where kg preserves no final newline for
  typed-from-empty buffers. Prefer explicit kg expectations when newline
  policy is not the behavior under test.
- Generated analysis artifacts such as `compile_commands.json`, `coverage/`,
  `*.plist`, object files, and `src/kg` are ignored; do not include them in
  reviews or commits.

## Lisp layer

- Fe (the embedded Lisp) lives in the `fe/` git submodule, pinned per
  `doc/fe-upstream.md`. kg compiles only `fe/fe.c`; `fe.h` may be included
  only by the `src/lisp_*.c` adapter implementation files and their private
  `src/lisp_internal.h` (which includes fe.h itself, being one of the
  standalone header-check units) — `make lisp-include-check` enforces
  that. Editor modules use `src/lisp.h` and stay free of `KG_USE_LISP`
  conditionals.
- The pin is a branch, not a SHA, and the branch carries kg-side changes to
  `fe.c` — fe is not pristine upstream. `doc/fe-upstream.md` lists every
  divergence; prefer kg's prelude in `src/lisp_prelude.c` over a new one.
- kg's Lisp is Emacs-shaped (`defun`, `lambda`, `setq`, `progn`, `let` with
  binding lists, backquote). Write examples, tests and docs that way; `=` is
  still assignment, not comparison.
- `WITH_LISP=1` is the default build; `make WITH_LISP=0` must reproduce the
  pre-Lisp editor. CI stage `.ci/ci-08-with-lisp-0.sh` enforces the disabled
  configuration; keep both configurations green.
- Lisp is trusted code: init files and packages run with full editor
  privileges, bounded by a step budget and C-g cancellation. Do not present
  it as a sandbox.
- PTY YAML supports `requires_feature: lisp` (skipped when `kg -V` reports
  `-lisp`) and `config_files:` mapping HOME-relative paths to contents for
  planting `init.el`/packages inside the isolated case HOME.
- User key bindings go through `src/keybind.c`, the single canonical
  key-sequence parser; only "C-c <key>" sequences are bindable.
