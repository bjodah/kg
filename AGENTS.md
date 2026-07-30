# kg agent notes

kg is a small Emacs-style terminal editor written in C23. Read `README.md` first for project goals and user-facing behavior.

## Repo shape
- `src/`: editor implementation
- `test/`: standalone regression tests and test stubs
- `doc/`: man page, screenshots, roadmap
- `Makefile`: build, test, install, release targets

## Build and test
- Build: `make` or `CC="ccache cc" make`
- Run tests: `make check`
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
- Seed inputs for the regex fuzzer are tracked in `test/fuzz-seeds/regex`
  and copied into the gitignored working corpus by `make fuzz-regex-seed`,
  which `make fuzz-regex-smoke` depends on. `make fuzz-regex-seed-replay`
  runs every tracked seed once without mutation. The input encoding is in
  the header comment of `test/fuzz_regex.c`.
- To iterate on one CI gate, run its script directly, e.g.
  `.ci/ci-01-*.sh`; shared defaults come from `.ci/ci-env.sh`.
- `CC` and `CFLAGS` are environment-overridable, e.g. `CC="ccache clang" CFLAGS="..." make`.
- Final green light comes from running `.ci/run-ci-steps.sh` (static
  analysis, sanitizers, compilation warnings as errors...). The runner
  dispatches numbered scripts `.ci/ci-01-*.sh` through `.ci/ci-11-*.sh`;
  run one directly when iterating on a specific phase. The step list is a
  glob, so a new `.ci/ci-NN-*.sh` joins the run with no runner change.
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
- When behavior changes, add or update a focused test under `test/`.
- Prefer the existing C harness for pure logic and `test/pty/*.yaml` for interactive editor behavior.
- If you change user-visible behavior or keybindings, update `README.md`
  and/or `doc/kg.1`; also update the built-in help table in `src/help.c`
  when a keybinding changes.
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
  `doc/fe-upstream.md`. kg compiles only `fe/fe.c`; only `src/lisp.c` may
  include `fe.h`. Editor modules use `src/lisp.h` and stay free of
  `KG_USE_LISP` conditionals.
- The pin is a branch, not a SHA, and the branch carries kg-side changes to
  `fe.c` — fe is not pristine upstream. `doc/fe-upstream.md` lists every
  divergence; prefer kg's prelude in `src/lisp.c` over a new one.
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
  planting `init.fe`/packages inside the isolated case HOME.
- User key bindings go through `src/keybind.c`, the single canonical
  key-sequence parser; only "C-c <key>" sequences are bindable.
