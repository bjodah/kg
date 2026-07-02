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
stubbing out prompt-driven escape hatches such as:

- `M-!` and `M-|`
- `C-x C-w` and `C-x i`
- buffer switching and help buffers
- interactive search and `M-x`

That makes the target safe to run on a normal workstation: the fuzzer never
executes shell commands and never writes outside the in-memory test state.

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
