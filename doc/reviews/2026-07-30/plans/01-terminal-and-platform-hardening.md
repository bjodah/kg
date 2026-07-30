# Plan 01 — Terminal trust boundary and platform hardening

## Goal

Prevent untrusted bytes from becoming terminal control sequences, then harden
the adjacent TTY, character-classification, geometry, and input paths: escape
injection through buffer text, filenames, Dired and status messages; short or
interrupted terminal writes; nonsensical terminal geometry; UB from negative
`char` values passed to `<ctype.h>`; and the valid byte lost after a malformed
UTF-8 input lead.

Do not combine this work with a new renderer or syntax architecture. The first
three phases are local security/correctness changes. Line numbers below were
re-verified against the working tree; re-read the function before editing.

## Read first

- `src/display.c`: `ab_append()` (36), `draw_window_rows()` (92),
  `draw_mode_line()` (378), the echo-area block in `editor_refresh_screen()`
  (549-591).
- `src/syntax.c`: `generic_keyword_scan()` (1577),
  `editor_update_syntax_row_only()` (1789); `src/dired.c`
  `dired_highlight()` (107).
- `src/tty.c`: `read_input_byte()` (134), `editor_read_utf8_seq()` (493),
  `get_cursor_position()` (525), `get_window_size()` (583),
  `apply_window_size()` (634).
- `src/def.h`: `tty_write()` (80, a `static inline` returning `void`),
  `write_all()`/`editor_write_fn` (796-798), UTF-8 helpers (654-735),
  `HL_NONPRINT` (93); `src/fileio.c` 47-73, the working short-write/EINTR loop
  with injectable syscall pointers, which Phase 3 reuses rather than reinvents;
  `src/shell.c` `shell_output_fits_echo()` (534) — see the Phase 2 correction.
- `test/test_tty.c`, `test/test_syntax.c`, `test/test_buffer.c`
  (`test_write_all`, 1226), and tmux-backed PTY YAML cases.

## Verified state of the defect

Buffer bytes reach the terminal via `draw_window_rows()` →
`ab_append(ab, c + j, 1)` (display.c:312 and :324). The only filter is
`hl[j] == HL_NONPRINT` (display.c:295), and that highlight is set in exactly one
place, `generic_keyword_scan()` (syntax.c:1664). Consequences:

1. **No syntax at all.** `editor_update_syntax_row_only()` returns early at
   syntax.c:1817 (`if (editor.syntax == NULL) return;`), leaving every byte
   `HL_NORMAL`. Extensionless files, `.txt`, and anything unmatched emit raw
   ESC/CSI/OSC.
2. **Custom highlighters.** Any syntax with a non-NULL `highlight` hook (Dired,
   YAML, Markdown, git commit/rebase) bypasses `generic_keyword_scan()` and
   never sets `HL_NONPRINT`.
3. **The `HL_NONPRINT` renderer itself injects.** display.c:298 computes
   `sym = (c[j] <= 26) ? ('@' + c[j]) : '?';` where `c` is `char *`. On a
   signed-char target every byte ≥ 0x80 is negative, passes `<= 26`, and yields
   `64 + (signed value)`. Buffer byte `0xDB` becomes `27` — a **raw ESC written
   by the code that exists to neutralise control bytes**. `0x80` becomes `0xC0`,
   `0xE0` becomes a space. This is reachable today: `isprint()` of a signed
   `char` `0xC3` is 0 in the C locale, so ordinary UTF-8 in a C-syntax buffer
   already takes this path (verified with a scratch probe against glibc).

`draw_mode_line()` interpolates `buf_display_name()` output — the filename —
unfiltered (display.c:396, 425-432). The echo area (display.c:562-580)
deliberately steps *past* CSI sequences so they reach the terminal intact;
`native_message` (lisp.c:380) and every
`editor_set_status_message("%s", <untrusted>)` caller feed it.

## Non-negotiable invariants

1. No byte originating in a file, filename, directory entry, Lisp string, or
   subprocess output is emitted as terminal syntax; only renderer-owned code
   emits ESC/CSI/OSC, and styling is a trusted operation, not embedded ANSI.
2. TTY frame writes either complete, retry, or report terminal loss.
3. Layout code never receives zero, negative, overflowed, or unreasonably large
   dimensions.
4. `<ctype.h>` is called only with `EOF` or an `unsigned char`, and never
   decides anything about bytes ≥ 0x80.

## Phase 1 — Introduce a single display escaping primitive

### Files

`src/display.c`; `src/def.h` only if a helper must be test-reachable;
`test/test_syntax.c` (cheapest home — it already links `stubs.o` +
`TEST_SRCS_OBJS`) or a new `test/test_display.c` plus a `Makefile` entry.

### Changes

Add an internal helper in `src/display.c`, matching the existing `abuf`
convention (`int` length, `int` return propagating the OOM flag):

```c
enum display_text_context {
	DISPLAY_BUFFER_TEXT,
	DISPLAY_FILENAME,
	DISPLAY_STATUS_TEXT,
};

static int ab_append_terminal_text(struct abuf *ab, const char *text,
    int len, enum display_text_context context);
```

All three contexts may behave identically at first; the enum exists so later
code cannot add unnamed exceptions. The helper must pass printable ASCII
(`0x20..0x7e`) unchanged; preserve valid UTF-8 glyphs, validated with the
existing `utf8_glyph_span_at()` (def.h:691) rather than re-deriving glyph
length; render C0 controls, DEL, and C1 controls (`0x80..0x9f`) visibly; never
emit a raw BEL, ESC, CSI, OSC introducer, or string terminator; make malformed
UTF-8 bytes visible one byte at a time; and never call locale-dependent
`isprint()` for a byte ≥ 0x80.

Recommended spelling, documented in a comment above the helper: `^@`..`^_` and
`^?` for `0x00..0x1f`/`0x7f`; `\xNN` (lowercase hex) for `0x80..0x9f` and
malformed bytes. Tabs stay a layout concern of `editor_update_row()`; newline
never appears inside one row. No Unicode control pictures — a font dependency.

Keep the append mechanics private. If a native test cannot otherwise reach the
classification, extract only the pure part and declare it in `def.h` — e.g.
`int display_escape_byte(unsigned char b, char *out);` writing the visible
spelling into `out[]` (≥ 5 bytes) and returning the byte count, or 0 when the
byte is safe as-is. `display.c` is in no test binary today, and a `test_display`
target would need a stub file that does **not** define
`editor_set_status_message()` or `buf_display_name()` — both live in
`test/stubs.c` and both are also defined by `display.c`/`bufmgr.c`. Prefer
testing the pure helper from `test_syntax`.

### Tests

Table-driven native tests: printable ASCII; every C0 byte, DEL, and C1 bytes
(`0x9b` in particular); `ESC [ 31 m`, OSC title (`ESC ] 0 ; x BEL`), OSC 52,
ST-terminated OSC; valid 2-, 3-, 4-byte UTF-8; stray continuation, truncated
lead, overlong (`C0 80`), surrogate (`ED A0 80`), out-of-range (`F5 80 80 80`)
— kg's helpers treat each malformed byte as a one-byte glyph, so assert that
policy explicitly; and `abuf` allocation failure (set `ab.oom`, expect return 0
and nothing appended). Assert that no output contains raw ESC or BEL, that valid
ordinary UTF-8 is byte-identical, and that every input byte is either preserved
in a valid glyph or represented visibly — none silently disappears.

## Phase 2 — Route every untrusted display path through escaping

### Files

`src/display.c`, `src/syntax.c`, `src/bufmgr.c` (line 978), `src/lisp.c`
(line 380), `test/pty/` new cases.

### Changes

In `draw_window_rows()`: keep renderer-generated SGR trusted (`\x1b[7m`,
`\x1b[27m`, `\x1b[39m`, the colour escape at display.c:318); replace **both**
`ab_append(ab, c + j, 1)` calls (312, 324) with the helper; delete the
`HL_NONPRINT` special case (295-306), whose `'@' + c[j]` arithmetic is the
injection above and which Phase 1 subsumes (either drop `HL_NONPRINT` from
`syntax.c` too, or keep it as colour only); recompute the visible-column bound
at display.c:232-244 from *escaped* widths, since the helper emits more than one
byte per source byte and a row of ESC bytes would otherwise overflow `win_w`;
and preserve tab expansion and selection boundaries — the reverse-video toggle
deferred on continuation bytes (display.c:286) must keep holding.

`draw_mode_line()`: escape `bname` (396) and any other dynamic marker; budget
width from escaped text, never truncating inside an escape spelling or a UTF-8
glyph; and fix the latent overread at 425-432, where `len` is `snprintf`'s
*would-be* length, so on a wide terminal (`win_w >= len > sizeof(status)`)
`ab_append(ab, status, len)` reads past `status[512]` — clamp with
`if (len >= (int)sizeof(status)) len = (int)sizeof(status) - 1;` before the
`win_w` clamp.

Echo area (549-591): remove the "recognise and step over CSI" loop; replace with
either (1) typed append operations for trusted styling plus escaped text
(preferred), or (2) plain escaped text, with callers losing inline colour until
the typed API lands. There is exactly one legitimate styling caller:
`src/bufmgr.c:978` formats `"\x1b[1m%s\x1b[22m"` around `names[i]`, a completion
candidate derived from a filename — simultaneously the reason the passthrough
exists and an untrusted interpolation. Convert it first, then delete the
passthrough. Confirm with `rg -n '\\x1b|\\033' src/*.c` that nothing else embeds
escapes (at time of writing: only `bufmgr.c:978`, plus renderer code in
`display.c`/`tty.c` and `kbd.c:1074`'s screen clear).

Dired must not depend on `dired_highlight()` to sanitise entry names — it does
not try. Its bytes flow through the same renderer-wide escaping.

**Correction to the previous draft:** there is *no* control-byte stripping in
`src/compile.c` or `src/shell.c` to preserve. `shell_output_fits_echo()`
(shell.c:534) is a *gate*: output containing `\n`, ESC, a byte `< 0x20`, or
`0x7f` is refused the echo area and put in `*Shell Command Output*` — raw.
Compilation output is stored raw as well. Keep the gate (it is a useful "one
clean line?" test) but do not describe it as defence in depth: rendering is now
the only place the invariant is enforced.

### PTY tests

Use `backend: tmux`. The harness captures with `tmux capture-pane -p`
(utils/pty_accept.py:456, 549) — **no `-e`** — so the capture is the *rendered*
screen, not the byte stream. Design assertions accordingly:

- extensionless file whose `initial:` contains `"\x1b[2J"` (double-quoted YAML
  supports `\x`): before the fix tmux wipes the pane; after it, the screen must
  still show the mode line and must contain `^[[2J` as text;
- a `config_files:` entry whose *name* contains ESC or BEL, opened with
  `C-x C-f`, so both the mode line and a status message carry it;
- a Dired listing (`C-x d`) of a directory holding such a name — copy the shape
  of `test/pty/dired-marks.yaml`, including `trailer_keys: [C-x, C-c]`;
- a `requires_feature: lisp` case planting an `init.fe` calling
  `(message "...\x1b[31m...")`.

Assert with `expected_screen_contains` (visible escaped spelling) plus
`expected_screen_not_contains` (evidence the sequence did not execute, e.g. the
mode line survived a `[2J`). Only if that is genuinely insufficient, extend
`utils/pty_accept.py` with an opt-in `expected_screen_bytes_not_contains` backed
by `capture-pane -e`; tmux-only, documented encoding, its own commit.

## Phase 3 — Make TTY writes complete

### Files

`src/def.h`, `src/tty.c`, `test/test_tty.c`, `Makefile` (possibly `EXTRA_tty`).

### Changes

The loop this phase used to ask for **already exists**: `write_all()`
(fileio.c:55, declared def.h:796) retries `EINTR`, handles short writes, and
turns a zero write into `EIO`; it writes through the injectable
`editor_write_fn` (def.h:797); and `test/test_buffer.c:1226` (`test_write_all`)
already covers EINTR, short write, permanent error, and zero write with mocks.
Do not add a parallel `tty_write_all()`.

The work is to turn `tty_write()` (def.h:80) from
`static inline void tty_write(const void *buf, size_t n)` — one `write()`,
result discarded — into a real function in `src/tty.c`,
`int tty_write(const void *buf, size_t n);` returning 0 on success and -1 on
terminal loss, keeping the `KG_FUZZ` no-op branch; and to give
`editor_refresh_screen()` (display.c:660) somewhere to put the failure: set
`running = 0` on -1 rather than recursing into another broken write or another
status message that would itself need a frame.

Linkage notes before moving it: callers are `src/display.c:660`,
`src/kbd.c:1074`, and `src/tty.c:212/213/684/750` — nothing else. No test binary
links `display.o` or `kbd.o`; `test_tty` links `tty.o`. `write_all` lives in
`fileio.o`, which `EXTRA_tty` does not list — either add `$(OBJDIR)/fileio.o`
there (it drags in more of the tree) or keep the retry loop local to `tty.c`.
Decide in the commit message; do not leave both. `test/fuzz_keypress` compiles
`tty.c` with `-DKG_FUZZ`, where `tty_write` is a no-op — keep that branch.

### Tests

In `test/test_tty.c`, point `editor_write_fn` at mocks copied from
`test/test_buffer.c:1160-1253`, and assert `tty_write()` returns 0 for a
short-write-plus-EINTR sequence and -1 for permanent error and zero write.
Restore `editor_write_fn = write;` at the end of each test.

## Phase 4 — Validate terminal geometry once

### Files

`src/tty.c`, `src/winmgr.c`, `test/test_tty.c`, `test/pty/` resize cases.

### Verified defects

`get_cursor_position()` parses the DSR reply with
`sscanf(buf + 2, "%d;%d", rows, cols)` (tty.c:572): it accepts a leading `-`,
ignores trailing junk, and overflows on long digit runs (UB).
`get_window_size()` (tty.c:594) tests `ws.ws_col == 0` but never `ws.ws_row`,
and calls `ioctl(1, TIOCGWINSZ, ...)` on hardcoded fd 1 rather than `ofd`.
`apply_window_size()` (tty.c:634) computes `editor.screenrows = rows - 2` with
no floor, so `rows == 1` gives `-1`. `win_reflow()` (winmgr.c:73) sets
`usable = win_total_rows - 1`, then `row_h = usable / n` and
`winlist[i].h = band_h - 1`, so a 1-row terminal yields `h == -1` and a mode
line drawn at terminal row 0 (`ESC[0;1H`).

### Changes

Add one checked boundary helper in `src/tty.c`:

```c
/* 1 with a usable size in *out_rows/*out_cols, 0 when the probe result is
 * not a plausible terminal size at all. */
static int normalize_window_size(int rows, int cols,
    int *out_rows, int *out_cols);
```

Document, as named constants beside it: `KG_MIN_ROWS` (3 — one text row, one
mode line, one echo row) and `KG_MIN_COLS` (8, so the mode line and the
`~`/logo paths have somewhere to land); `KG_MAX_ROWS`/`KG_MAX_COLS` keeping
`rows * cols` and the `int` `abuf` arithmetic (display.c:44) far from `INT_MAX`
— 10000 each is generous and safe. Prefer clamping below-minimum sizes up over
adding a "terminal too small" frame: clamping keeps `win_reflow()` total and
needs no new render path.

Replace the `sscanf` with a hand-rolled unsigned parse requiring decimal digits
only in both fields, no sign, exact `ESC [ <rows> ; <cols> R` framing, no
overflow, no trailing bytes. Apply `normalize_window_size()` on **both** the
ioctl and DSR paths, before `apply_window_size()` (tty.c:634; callers
`probe_window_size()` 679 and `update_window_size()` 703). Add defensive clamps
in `win_reflow()` so `h >= 0` and `w >= 1` for every active window even if a
caller bypasses it.

### Tests

Native: the DSR parser is `static`. Either give it external linkage as
`kg_parse_cursor_report()` declared in `def.h`, or test
`normalize_window_size()` the same way. Table: `"\x1b[24;80R"`, `"\x1b[0;0R"`,
`"\x1b[-1;80R"`, `"\x1b[99999999999;80R"`, `"\x1b[24;80Rjunk"`, `"\x1b[24R"`,
`"\x1b[;80R"`, empty.

PTY: the harness accepts a `RESIZE=r,c` token on **both** backends
(pty_accept.py:157 pexpect, :537 tmux), so a resize matrix needs no harness
change. The schema requires `dimensions:` entries to be positive integers
(pty_accept.py:316), so cover zero only in the native test. Matrix: start
`dimensions: [24, 80]`, then `RESIZE=1,1`, `RESIZE=2,5`, `RESIZE=3,8`, back to
`RESIZE=24,80`, saving at the end; repeat with a `C-x 2` / `C-x 3` split active.
Acceptance is that kg survives and saves correctly — enough to fail today's
build if the clamps are wrong.

## Phase 5 — Audit character classification

### Files

`src/syntax.c`, `src/autocomplete.c`, `src/cmd.c`, `src/search.c`,
`src/bufmgr.c`, `src/word.c`, `src/localvars.c`, `src/lisp.c`. Enumerate with:

```sh
rg -n '\b(isalnum|isalpha|isdigit|isprint|isspace|ispunct|iscntrl|isupper|islower|isxdigit|isblank|tolower|toupper)\s*\(' src fe
```

### Correction to the previous draft

kg **never calls `setlocale()`** — the only mention in the tree is the comment
at `src/width.c:3` explaining why `wcwidth(3)` is unusable for that reason. The
process always runs in the `"C"` locale, so a "UTF-8 locale lane" would test
nothing. Drop it. The real hazards are:

1. **Negative `char`.** Passing a negative value other than `EOF` to `ctype` is
   undefined; glibc survives it, other libcs and sanitizers need not. Confirmed
   uncast sites: `autocomplete.c:68`, `syntax.c:1590`, `syntax.c:1664`,
   `syntax.c:1736`, `syntax.c:681` (`is_separator(int c)` — audit callers),
   `cmd.c:1069`, `search.c:489`, `bufmgr.c:679`, `bufmgr.c:1413`.
   `word.c:489`, `word.c:922-927`, `lisp.c:820` and the `localvars.c` sites
   already cast or take `unsigned char`; verify rather than assume.
2. **Semantics, not just UB.** In the C locale `isprint(0xC3)` is **0**, so
   `syntax.c:1664` marks every non-ASCII byte `HL_NONPRINT` in any buffer using
   the generic scanner (C, Python, shell, …), feeding the broken caret renderer
   above. Casting alone does not fix this; the condition must become "ASCII
   control byte":

   ```c
   unsigned char ch = (unsigned char)*p;
   if (ch < 0x80 && !isprint(ch)) { ... }
   ```

   Everything ≥ 0x80 is left to Phase 1's helper, which knows about UTF-8.
3. **`int` key codes.** `bufmgr.c:679`/`:1413` and `cmd.c:1069` call `isprint()`
   on values that may be soft key codes above 0xFF (def.h:140+). Guard with
   `c >= 0 && c <= 0xFF` before the cast.

### Changes and tests

Add `static inline int ascii_is_digit(unsigned char)`, `ascii_is_space()`,
`ascii_is_print()` to `def.h` and use them wherever the grammar is ASCII by
definition (syntax scanning, local-variable parsing). Where libc `ctype`
genuinely belongs, cast to `(unsigned char)` at the call. Never pass a codepoint
above `UCHAR_MAX` to a byte `ctype` function.

Add a `-funsigned-char` build lane. This is not cosmetic: it currently changes
rendering (`c[j] <= 26` at display.c:298 stops matching non-ASCII, and
`isprint(*p)` stops sign-extending), so it is a real differential. Run
`test_syntax` under `-fsigned-char` and `-funsigned-char` on identical input
containing UTF-8, ESC, and DEL; the results must agree. Add a narrow CI checker
for uncast `ctype` calls **only after** the tree is clean, as a new
`.ci/ci-NN-*.sh` (the runner globs, so no runner change is needed).

## Phase 6 — Preserve the byte after malformed UTF-8 input

### Files

`src/tty.c`; the four call sites `src/kbd.c:1100`, `src/search.c:501`,
`src/bufmgr.c:687`, `src/bufmgr.c:1423`; `test/stubs_buffer.c:38` (the stub must
track any signature change); `test/test_tty.c`.

### Verified defect

`editor_read_utf8_seq()` (tty.c:493) reads `utf8_lead_extra()` continuation
bytes eagerly; on the first non-continuation it returns 0 (tty.c:507-509) and
the byte it already consumed is gone. `E2 41` swallows the `A`. Each caller
already drops the malformed lead; only the *following* byte needs rescuing.

### Two options — pick one and justify it in the commit

**A. Out-parameter (recommended).**
`int editor_read_utf8_seq(int fd, int lead, char *seq, int *pushback);` with
`*pushback == -1` when nothing was over-read, otherwise the byte value. Each
caller handles it locally: `kbd.c` re-enters its dispatch, the minibuffer
helpers re-run their key switch. Update `test/stubs_buffer.c:38`. No global
state, identical behaviour under `KG_FUZZ`, directly unit testable.

**B. Push back onto `pending_input`** via
`static int pending_input_prepend_byte(unsigned char b);`. Four traps first:
`read_input_byte()` (tty.c:138) consults `pending_input` only when
`fd == STDIN_FILENO`, so the fuzzer and pipe-based tests would silently skip the
pushback; the same function frees the queue and nulls the pointer when it drains
(tty.c:141-146), so the helper must cope with `pending_input == NULL`;
`reserve_pending_input()` sits inside `#ifndef KG_FUZZ` (tty.c:102-132), so a
helper using it must be guarded the same way or the fuzz build breaks; and
`editor_read_raw_byte()` goes through `read_key_common()` (tty.c:456-473), which
calls `macro_on_key()`, so a byte read, pushed back, and read again is
**recorded into a keyboard macro twice** unless the second record is suppressed.

Either way, handle queue/allocation failure without silently losing
synchronisation, and keep the malformed-lead policy (drop it) unchanged.

### Tests

`test/test_tty.c` links `stubs.o` + `tty.o` and defines its own local stubs at
lines 11-15 (`macro_next_key`, `macro_on_key`, …) — extend those rather than
adding a stub file. With option A, drive the reader with a `pipe()` whose read
end is the `fd`; `read_key_byte()` (tty.c:426) treats `nread == 0` as a VTIME
timeout and loops forever, so a pipe at EOF hangs the test — keep the write end
**open** and write exactly the bytes the test consumes. With option B you must
`dup2()` the read end onto `STDIN_FILENO` because of the guard above.

Cases: `E2 41` (the `A` must come out next, exactly once); `E2` then `C-g`; two
malformed leads back to back; a truncated 4-byte lead followed by a valid 2-byte
glyph; a lone stray continuation byte. Add one PTY case: plain buffer, a bare
`0xE2` followed by `A`, `expected_saved` containing the `A`, using `C-q`
quoted-insert to get the raw lead in reliably. Keep `key_delay` at the default
0.05 or higher — below 0.03 kg enters `editor.paste_mode` (kbd.c:533) and
changes behaviour.

## Documentation

`doc/kg.1` gets the control rendering policy and the minimum terminal size;
`doc/FUZZING.md` gets hostile terminal-sequence corpus guidance; `README.md`
only if the visible control-byte spelling is user-relevant. `src/help.c` is
unaffected (no keybinding changes). Touch `AGENTS.md` (`CLAUDE.md` is a symlink
to it) only if a new PTY assertion or the ctype rule becomes standing workflow.

## Final acceptance

```sh
make check
make WITH_LISP=0 clean all check
make fuzz-keypress-smoke
.ci/run-ci-steps.sh --parallel
```

`.ci/run-ci-steps.sh --status` reports progress without blocking; poll that
instead of watching the process table.

Manual smoke: open a file containing an OSC title sequence under tmux; visit a
directory containing a filename with ESC and BEL; trigger an error message
interpolating that filename; verify the terminal title does not change and all
controls read as text.

## Risks and rollback points

- Escaping changes width and truncation. Land buffer rows, mode line, and echo
  area in **separate commits** so a display regression is easy to bisect.
- Removing the `HL_NONPRINT` rendering changes what a C file containing UTF-8
  looks like today (it currently shows garbage). That is a fix, but say so in
  the commit message and update any screen assertion it touches.
- Do not partially retain "ANSI in status strings"; that recreates the trust
  boundary through interpolation. Do not put terminal-safety decisions back into
  syntax highlighters: buffers with no syntax and buffers with custom
  highlighters are exactly the cases that exposed the defect.
- Phase 5's `isprint` change and Phase 1's helper overlap. Land Phase 1 first so
  the syntax change has somewhere safe to fall through to.
