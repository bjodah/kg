# kg agent notes

kg is a small Emacs-style terminal editor written in C99. Read `README.md` first for project goals and user-facing behavior.

## Repo shape
- `src/`: editor implementation
- `test/`: standalone regression tests and test stubs
- `doc/`: man page, screenshots, roadmap
- `Makefile`: build, test, install, release targets

## Build and test
- Build: `make`
- Run tests: `make check`
- Clean binaries/objects: `make clean` or `make distclean`
- `make check` now runs two layers:
  - native unit tests in `test/test_*.c`
  - PTY-backed acceptance cases from `test/pty/*.yaml` via `utils/pty_accept.py`

## Editing expectations
- Keep changes small and local; this codebase values minimalism.
- Match existing C style: C99, tabs, short helper functions, Linux-kernel-like brace/layout conventions.
- Avoid new dependencies unless explicitly requested.
- When behavior changes, add or update a focused test under `test/`.
- Prefer the existing C harness for pure logic and `test/pty/*.yaml` for interactive editor behavior.
- If you change user-visible behavior or keybindings, update `README.md` and/or `doc/kg.1`.

## Useful context
- `doc/TODO.md` tracks planned editor features and known technical debt.
- Tests are simple native binaries; prefer extending that harness before inventing new infrastructure.
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
