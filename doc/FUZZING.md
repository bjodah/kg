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

## Native fuzz targets

Every native target has the same three Make targets:

- `make fuzz-<name>` builds it with libFuzzer, ASan and UBSan.
- `make fuzz-<name>-seed` copies its tracked seeds into the gitignored
  working corpus.
- `make fuzz-<name>-smoke` seeds it and fuzzes for five seconds (override
  that budget with `FUZZ_MAX_TOTAL_TIME`).

The available names are `keypress`, `syntax`, `dirlocals`, `regex`,
`localvars`, `compile-parse`, `lsp-json`, `width`, and `keybind`.
`make fuzz-smoke` runs every smoke target;
`make fuzz-seed` only prepares their corpora.  For a longer campaign, run the
binary directly after its seed target, for example:

```bash
make fuzz-syntax-seed
mkdir -p test/fuzz-artifacts/syntax
./test/fuzz_syntax -artifact_prefix=test/fuzz-artifacts/syntax/ \
	test/fuzz-corpus/syntax
```

Use the same shape for every other target by replacing `syntax` in the path
and binary name.  The smoke budget is a regression check, not a substitute
for a longer campaign.

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

### Seeding it with hostile terminal input

The keypress fuzzer feeds raw bytes through the real key decoder, so it is
also the cheapest way to attack the terminal trust boundary.  Two families
are worth having in the corpus, because neither is reachable by typing:

- **Sequences kg must never echo.**  A buffer or a prompt that ends up
  holding `ESC [ 2J`, `ESC ] 0 ; x BEL`, OSC 52 (clipboard), a
  string-terminator-framed OSC, or a raw C1 introducer (`0x9B` for CSI,
  `0x9D` for OSC, `0x90` for DCS).  Nothing kg draws may contain these
  bytes: everything untrusted goes through `display_glyph_at()`, which
  spells them `^[`, `^G` and `\xnn`.  A frame containing a raw ESC that
  the renderer did not emit itself is the bug.
- **Byte sequences that are not characters.**  Truncated leads (`E2`
  alone, `F0 9F`), overlong forms (`C0 80`), leads no sequence starts with
  (`C0`, `C1`, `F5`..`FF`), stray continuation bytes, and a malformed lead
  immediately followed by an ordinary key — that last one is what
  `editor_read_utf8_seq()` used to swallow.

`test/pty/terminal-escape-*.yaml` and
`test/pty/malformed-utf8-keeps-next-key.yaml` are the confirmation-bar
versions of both families; a PTY case can send one raw byte with the
`BYTE=e2` key token.

Run a longer campaign with a corpus directory:

```bash
mkdir -p test/fuzz-corpus/keypress test/fuzz-artifacts/keypress
./test/fuzz_keypress -artifact_prefix=test/fuzz-artifacts/keypress/ \
	test/fuzz-corpus/keypress
```

libFuzzer will leave a reproducer file behind when it finds a crash, an OOM
or a timeout.  Without `-artifact_prefix` it drops that file in the working
directory, where it is untracked and unignored; the `fuzz-*-smoke` targets
pass the prefix above, and `test/fuzz-artifacts/` is gitignored.  Note the
trailing slash — it is what makes libFuzzer treat the value as a directory
— and create the directory first.  Replay the input directly:

```bash
./test/fuzz_keypress test/fuzz-artifacts/keypress/crash-*
```

When triaging a crash file:

1. Replay it against the current `test/fuzz_keypress` binary.
2. Check whether the same failure reproduces against `src/kg` in a PTY.
3. If it does not, keep it as a harness artifact or a future investigation,
   not as a user-facing regression.

## Syntax fuzzer

`make fuzz-syntax` builds a libFuzzer target around real row construction and
syntax highlighting.  Its first input byte chooses a syntax mode; `C`, `P`,
`S`, `J`, `R`, `M`, `L`, `Y`, `K`, `T`, and `H` name C, Python, Shell,
JavaScript, Rust, Markdown, Lisp, YAML, Makefile, TypeScript, and HTML.
Other first bytes cover the rest of the registry.  The remaining bytes are
file contents split at newlines and inserted only after the mode is selected.

That ordering is intentional: it reaches malformed syntax during file load,
including the Shell trailing-backslash overflow that `fuzz_keypress` could
not reach because it fuzzes an unhighlighted `fuzz.txt` buffer.

Bytes after the first `0xff` in the input -- never valid UTF-8, so plain
source seeds are unaffected -- are an edit script.  Each operation is a
two-byte position, a delete length, an insert length, and that many bytes of
replacement text, applied through `kg_buffer_replace()`, so the incremental
rescan (`syntax_after_edit()`) sees splits, joins, and rewrites of rows in
every mode.

Two oracles then run.  Every row must have a sufficiently sized highlight
array containing only defined highlight values.  Then the incrementally
maintained highlight bytes are compared against a from-scratch
`syntax_rebuild()` of the same text: a rescan that stopped too early or
leaked comment state is a byte-for-byte disagreement, printed with its row,
offset, and both values.  The same harness builds against the selected
syntax backend, so a Tree-sitter build exercises its load/rebuild path too.

Run its short CI-equivalent smoke test or a local campaign with:

```bash
make fuzz-syntax-smoke
FUZZ_MAX_TOTAL_TIME=300 make fuzz-syntax-smoke
```

## LSP JSON fuzzer

`make fuzz-lsp-json` targets `src/json.c` alone: the parser that eats a
language server's stdout, which is the least trusted input kg reads.  The
whole input is one candidate document.  A rejected document must report an
error offset inside the input; an accepted one is walked with every
accessor -- wrong-kind and out-of-range calls included, since the header
promises they answer rather than crash -- then re-serialised through the
builder half, reparsed, and compared node for node.  A roundtrip
disagreement means one half of the module misreads what the other wrote.

## Width fuzzer

`make fuzz-width` targets `src/width.c`, the one seam every untrusted byte
crosses on its way to the terminal.  Every byte offset of the input is asked
for its glyph, including offsets inside UTF-8 sequences, and the target
asserts the module's contract: spans stay inside the buffer, escape
spellings fit their field with width equal to length, and the per-byte
measurement loop agrees with the per-glyph walk about the total display
width -- the agreement between how a run is measured and how it is drawn.

## Keybind fuzzer

`make fuzz-keybind` targets the user key-binding seam (`src/keybind.c`,
`src/keymap.c`, `src/keyevent.c`): the canonical parser for the sequences
init.el may bind.  The input is cut at NULs into strings; each one's first
byte picks parse, bind, unbind, lookup, or a raw `key_parse()` roundtrip.
Accepted sequences must canonicalise idempotently, a successful bind must be
visible to lookup and a successful unbind invisible, and a parsed key must
survive `key_format()` unchanged.

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
