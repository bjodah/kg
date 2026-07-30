# Plan P0 — Evidence, baselines, and budgets

Land this plan before Plans 01–15. It changes no editor behavior. Its job
is to make every later "the fix did not break what already worked" claim
checkable, and to remove the two purely mechanical blockers (ratchet
headroom, hosted-CI truthfulness) that would otherwise stall Wave 1.

Read first: `CLAUDE.md`, `README.md`, `utils/pty_accept.py`,
`.ci/ci-env.sh`, `Makefile` (check targets), `test/pty/` case examples
(`dired-marks.yaml`, `self-insert-utf8-undo.yaml`).

## Phase 1 — Buffer/window characterization PTY cases

Verified gap: across 224 PTY cases there is exactly one case each
exercising `C-x 2`, `C-x 3`, `C-x o`, and none for `C-x b`, `C-x C-b`,
`C-x k`, `C-x 0`, `C-x 1`, nor for two windows showing one buffer. These
paths are exactly what Plan 09's flag days rewrite, so they need a net
first.

Add PTY cases (`test/pty/`), each with a deliberate `filename:`:

1. `bufswitch-basic.yaml` — open file A, `C-x b` to a new buffer, type,
   `C-x b` back, verify both saved outputs.
2. `bufswitch-list.yaml` (`backend: tmux`) — `C-x C-b`, assert
   `expected_screen_contains` lists both buffers.
3. `bufkill.yaml` — modify buffer, `C-x k`, answer the discard prompt both
   ways in two cases.
4. `win-close-this.yaml` / `win-close-others.yaml` — `C-x 2`, edit in one
   window, `C-x 0` / `C-x 1`, verify the surviving window's point and the
   saved file. Note: `win_delete_others()` (`src/winmgr.c:317-327`) does
   not call the save-state helper other window commands use; characterize
   what it *does* today, do not fix it here.
5. `two-windows-one-buffer.yaml` — `C-x 2`, edit in window 1, `C-x o`,
   verify the edit is visible in window 2 (shared text), and that point
   differs per window.
6. `goal-column-across-switch.yaml` — mark `xfail: true`:
   `editor.desired_visual_col` is owned by neither buffer nor window, so
   the goal column leaks across `C-x b`/`C-x o`. Plan 09 phase 5 fixes it
   and flips this case.

Keep `key_delay >= 0.05` (paste-mode threshold, `CLAUDE.md`).

## Phase 2 — Truthful hosted `make check` (= Plan 07 phase 1a)

Verified facts:

- `utils/pty_accept.py:56` pins `EMACS = "/opt-3/emacs-31-lucid/bin/emacs"`;
  17 of 224 cases use `oracle: emacs` and a missing oracle is an ERROR,
  not a skip.
- 71 cases use `backend: tmux`; `pexpect` and `yaml` are module-scope
  imports; the GitHub workflow installs none of these.
- `Makefile:220` invokes `python`, not `python3`.

Changes:

1. `utils/pty_accept.py`: resolve the Emacs oracle from `--emacs`, then
   `$KG_PTY_EMACS`, then `shutil.which("emacs")`, then the `/opt-3` pin.
   Add `--require-tools` (used by CI) so a missing oracle/tmux fails the
   run loudly instead of silently skipping; without the flag, oracle cases
   `SKIP` with a printed count.
2. `Makefile`: `python` → `python3`; export `KG_PTY_EMACS` pass-through.
3. `.github/workflows/`: install `tmux`, `python3-pexpect`, `python3-yaml`,
   `emacs-nox`; pass `--require-tools`. Print tool versions.
4. Investigate first (do not assume): whether hosted `make check` is
   currently red — inspect the latest Actions run before changing the
   workflow, and record the finding in the PR description.

Regression: a workflow run where `tmux` is deliberately absent must fail
with a message naming the missing tool.

## Phase 3 — One deliberate complexity re-baseline

Verified: `scc` over `src/` measures exactly `SCC_COMPLEXITY_MAX` (4208);
`editor_process_keypress` measures exactly
`PMCCABE_FUNCTION_COMPLEXITY_MAX` (120); Fe's scc total is 171 vs. a cap
of 172. Every plan would otherwise fight `.ci/ci-01` on its first commit.

This is the *one* sanctioned ratchet change of the program:

1. Extract obviously separable arms from `editor_process_keypress`
   (`src/kbd.c:507`) into static helpers until pmccabe reports ≤ 110,
   removing the `__attribute__((optimize("O0")))` workaround if the
   analyzer allows (investigate what it suppresses first; if it masks a
   real `-Wanalyzer-out-of-bounds` finding, file that as a bug instead).
2. Do the same for the top one or two scc contributors if extraction is
   mechanical; otherwise raise `SCC_COMPLEXITY_MAX` by at most 2% with a
   commit message that cites this plan.
3. State the resulting headroom numbers in the commit message. Later
   plans budget against them; nobody raises a ratchet again without a
   reviewed exception.

`make check` and `WITH_LISP=0` must stay green; behavior-free refactor.

## Phase 4 — Fresh measurement baselines

1. Regenerate coverage on a full `make check` run and record line/function
   rates per file. Verified: the checked-in artifact covers 20 of 28
   `src/*.c` files (`lisp.c`, `compile.c`, `dired.c`, `help.c`,
   `keybind.c`, `mode.c`, `localvars.c`, `width.c` absent), so the quoted
   55.3%/68.2% is not the shipped code. Coverage pitfall: `PTY_JOBS=8`
   cases share one instrumented binary writing the same `src/*.gcda`; use
   `PTY_JOBS=1` or per-case `GCOV_PREFIX` for the baseline run.
2. Record `make check` wall time, per-suite timings, and the differential
   target's case rate in a dated file under `doc/reviews/2026-07-30/`
   (machine-readable, one JSON or TSV).
3. Do not add new analyzers or ratchets here.

## Phase 5 — Small hygiene with immediate payoff

1. `.gitignore`: `test/fuzz_dirlocals`, `test/fuzz_regex`,
   `test/fuzz_localvars` are untracked-but-unignored build outputs (only
   `test/fuzz_keypress` is ignored today). Add them.
2. Verify fuzz crash artifacts land in the already-ignored
   `test/fuzz-artifacts/` (pass `-artifact_prefix`); today crashes land in
   the working directory.

## Explicitly out of scope

- Any behavior fix (Plans 01–06).
- New CI stages, per-symbol ratchets, mutation testing (Plan 07).
- Golden-frame capture infrastructure beyond `expected_screen_contains`
  cases: if a Plan 01 rendering fix needs a before/after frame, capture it
  in that plan with a tmux case, not with new tooling here.

## Definition of done

- The six characterization cases run green (`xfail` case reports XFAIL).
- Hosted `make check` is demonstrably truthful (tool-missing run fails
  loudly; a green run really executed the oracle and tmux cases).
- Ratchets have stated headroom; the O0 workaround is understood.
- Baselines are committed as dated artifacts.
- `make check` and `WITH_LISP=0 make` green; no user-visible change.
