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
- `CC` and `CFLAGS` are environment-overridable, e.g. `CC="ccache clang" CFLAGS="..." make`.
- Final green light comes from running `.ci/run-ci-steps.sh` (static
  analysis, sanitizers, compilation warnings as errors...). The runner
  dispatches numbered scripts `.ci/ci-01-*.sh` through `.ci/ci-07-*.sh`;
  run one directly when iterating on a specific phase.
- CI parallelism is controlled with `JOBS` (default: `nproc`), e.g.
  `JOBS=8 .ci/run-ci-steps.sh`. Shared CI defaults live in
  `.ci/ci-env.sh`.

## Editing expectations
- Keep changes small and local; this codebase values minimalism.
- Match existing C style: C23, tabs, short helper functions, Linux-kernel-like brace/layout conventions.
- Avoid new dependencies unless explicitly requested.
- When behavior changes, add or update a focused test under `test/`.
- Prefer the existing C harness for pure logic and `test/pty/*.yaml` for interactive editor behavior.
- If you change user-visible behavior or keybindings, update `README.md`
  and/or `doc/kg.1`; also update the built-in help table in `src/help.c`
  when a keybinding changes.

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
- `dimensions: [rows, cols]` is available for viewport-sensitive cases.
- tmux-backed cases can assert visible screen content with `expected_screen_contains` and `expected_screen_not_contains`.
- Known discrepancies can be checked in as `xfail: true`; `XPASS` fails `make check` so expectations get cleaned up once behavior changes.
- Key tokens in PTY YAML are literal unless named. Use `SPC` for an
  actual space key, `RET` for Enter, `C-?` for Backspace, and `C-q`
  followed by the next token for quoted input.
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
