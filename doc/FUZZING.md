# Fuzzing and Crash Triage

## Crash capture

For a one-off segfault, first rebuild with debug info and sanitizers:

```bash
make clean
CC=clang CFLAGS='-O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer' make
```

If the crash still reproduces, use one of these:

```bash
ulimit -c unlimited
./src/kg
gdb ./src/kg core
```

```bash
gdb --args ./src/kg <file>
```

On Linux, `rr` is the best next step for an intermittent TTY bug:

```bash
rr record ./src/kg <file>
rr replay
```

If the failure needs a real terminal, reproduce it through the PTY test
runner or under `script`/`tmux` rather than piping stdin directly; kg's raw
mode expects a TTY.

## Native keypress fuzzer

`make fuzz-keypress` builds a libFuzzer target around
`editor_process_keypress()`.  It exercises the real editing code paths while
feeding raw byte streams through `tty.c`'s real key decoder.  The harness
still stubs out prompt-driven escape hatches such as:

- `M-!` and `M-|`
- `C-x C-w` and `C-x i`
- buffer switching and help buffers
- interactive search and `M-x`

That makes the target safe to run on a normal workstation: the fuzzer never
executes shell commands and never writes outside the in-memory test state.

The current harness is intentionally conservative about what fuzz findings
mean:

- a crash found only by `test/fuzz_keypress` is a triage candidate
- a crash that also reproduces under `utils/pty_accept.py`, `script`,
  `tmux`, `gdb --args ./src/kg ...`, or `rr record ./src/kg ...` is an
  actionable editor bug

Do not check in a speculative fix or regression test for a fuzz-only crash
unless a real TTY/PTY reproducer exists.  The native fuzzer is for coverage
and candidate discovery; the PTY layer is the confirmation bar.

Build and smoke-test it with:

```bash
make fuzz-keypress
make fuzz-keypress-smoke
```

Run a longer campaign with a corpus directory:

```bash
mkdir -p test/fuzz-corpus/keypress
./test/fuzz_keypress test/fuzz-corpus/keypress
```

libFuzzer will leave a reproducer file behind when it finds a crash.  Replay
that input directly:

```bash
./test/fuzz_keypress crash-*
```

When triaging a crash file:

1. Replay it against the current `test/fuzz_keypress` binary.
2. Check whether the same failure reproduces against `src/kg` in a PTY.
3. If it does not, keep it as a harness artifact or a future investigation,
   not as a user-facing regression.

## PTY-level fuzzing

The native fuzzer is the fast coverage engine.  For full end-to-end terminal
coverage, keep a second slower layer that launches `kg` under a PTY inside a
throwaway directory.

Minimum containment:

- create a fresh temp directory per run
- set `HOME` to that directory
- open only files rooted in that directory
- run with `-R` when the bug under test does not require writes

For stronger containment, wrap the binary with a sandbox such as `bwrap` or
`firejail` and mount only the temp directory read-write.
