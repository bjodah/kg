# Portability, security, and reliability review

Scope: kg, Fe, and tiny-regex-c as present on 2026-07-30. This was a
read-only source audit focused on trust boundaries, integer/size boundaries,
signals/processes/filesystem races, terminal behavior, locale/alignment, and
public-API misuse. Severity describes impact in plausible use, not remote
exploitability; confidence describes the source-level diagnosis.

## Ranked findings

### 1. kg sends untrusted file and filename bytes to the terminal verbatim

- **Severity:** high
- **Confidence:** high
- **Evidence:** `src/syntax.c:1808-1818` initializes every byte as
  `HL_NORMAL` and returns without marking control bytes when a buffer has no
  syntax mode. `src/display.c:278-325` appends every `HL_NORMAL` byte directly
  to the terminal output. The mode line likewise copies a filename-derived
  display name at `src/display.c:392-432`. Dired's custom highlighter at
  `src/dired.c:107-127` does not classify controls in entry names. Finally,
  the echo renderer deliberately recognizes arbitrary ESC/CSI bytes and emits
  the whole prefix unchanged at `src/display.c:552-590`; many status messages
  interpolate filenames (for example `src/fileio.c:506-511`).
- **Impact:** opening an extensionless/unrecognized file containing an OSC 52
  sequence can attempt clipboard replacement; CSI/OSC sequences can alter the
  screen, title, hyperlinks, or terminal-dependent features. A hostile
  filename can inject through the mode line, dired, and status messages even
  if file contents have a syntax mode. The shell-output path noticed this
  boundary and rejects controls (`src/shell.c:526-560`), but the general
  renderer does not enforce the same invariant.
- **Reproducer/test direction:** create an extensionless file whose row is
  `ESC ] 52 ; c ; <base64> BEL`, run kg under tmux, and assert the raw pane
  stream never contains that OSC sequence. Repeat with those bytes in a
  filename displayed by dired and the mode line. Use a harmless terminal-title
  OSC in manual testing.
- **Fix direction:** make terminal escaping a renderer-wide invariant,
  independent of syntax highlighting. Render C0/C1, ESC, DEL, invalid UTF-8,
  and preferably bidi controls visibly. Do the same for filenames and status
  text. Replace “status text may contain ANSI” with typed trusted markup
  fragments (or a very small internal styling API), so ordinary strings are
  always escaped by construction.

### 2. Fe's Fex file objects permit double-close/use-after-close and leak files

- **Severity:** high
- **Confidence:** high
- **Evidence:** `fe/fex_io.c:34-37` calls `fclose()` on the raw `FILE *` but
  leaves the Fe object pointing at it. Every subsequent read/write/close passes
  that stale pointer back into stdio (`fe/fex_io.c:51-65`,
  `fe/fex_io.c:75-90`), which is undefined behavior. Conversely, the Fex GC
  callback explicitly does nothing for `FexTFile` at `fe/fex.c:12-30`, so an
  unreachable open file is never closed. The predefined stdin/stdout/stderr
  objects at `fe/fex_io.c:29-31` are indistinguishable from owned files and can
  also be closed.
- **Impact:** trusted extension code can crash or corrupt libc state with
  `(close-file f)` twice or by reading/writing `f` after close. Loops which
  open and discard files exhaust descriptors. Richer package ecosystems make
  both mistakes more likely.
- **Reproducer/test direction:** add script/API tests for double close, use
  after close, GC of hundreds of unclosed files under a low `RLIMIT_NOFILE`,
  and GC of standard streams. Run the first cases under ASan/Valgrind, though
  libc may diagnose a double `fclose` before a sanitizer does.
- **Fix direction:** store a heap-owned handle structure `{ FILE *fp; bool
  owned; bool closed; }`, validate it in every operation, atomically null the
  pointer before/around close, and close only owned live handles in the GC
  callback. Decide and document whether closing a closed handle is idempotent
  or a Lisp error.

### 3. tiny-regex-c's advertised byte-storage API has alignment undefined behavior

- **Severity:** high on strict-alignment targets; medium on x86
- **Confidence:** high
- **Evidence:** the public API accepts `unsigned char *storage`
  (`fe/tiny-regex-c/re.h:107-111`). `re_compile_to()` immediately casts that
  pointer to `regex_t *` and performs `unsigned short` member stores
  (`fe/tiny-regex-c/re.c:582-605`, with the first stores from
  `fe/tiny-regex-c/re.c:608` onward). `re_compile_checked()` also uses an
  unaligned-type `unsigned char temp_buffer[]` and casts it
  (`fe/tiny-regex-c/re.c:359-395`), and `re_compile()` uses a static
  `unsigned char[]` (`fe/tiny-regex-c/re.c:848-854`). Nothing in the header
  states or enforces an alignment precondition.
- **Impact:** a valid call such as passing `raw + 1` is undefined behavior and
  may trap on ARM/MIPS/SPARC or under alignment sanitization. Current stack and
  static buffers are not guaranteed by C to have `regex_t` alignment either.
  `malloc` happens to align the Fex allocation, while kg's struct member
  requires inspection of that struct's alignment rather than relying on the
  API.
- **Reproducer/test direction:** compile with UBSan alignment checks and call
  `re_compile_checked("a", ..., raw + 1, ...)`; also build/run on a
  strict-alignment emulator.
- **Fix direction:** best is a byte-coded program accessed with `memcpy`/explicit
  little structures rather than typed overlays. A smaller change is to expose
  an alignment requirement and aligned storage type, use `_Alignas(regex_t)`
  for internal buffers, reject misaligned external storage, and add
  `RE_STORAGE_ALIGNMENT`. Avoid making the private `regex_t` layout part of
  ABI accidentally.

### 4. kg's external-change protection can silently miss and overwrite changes

- **Severity:** medium-high (data loss)
- **Confidence:** high
- **Evidence:** the snapshot retains only whole-second `st_mtime` and size
  (`src/fileio.c:20-31`, fields at `src/def.h:317-319`), and
  `file_state_differs()` compares only those two fields
  (`src/fileio.c:34-45`). A same-length rewrite with preserved/rounded mtime is
  invisible. A failed `stat()` also returns “unchanged,” so deletion or a
  transient lookup failure gets no conflict warning. The check occurs at
  `src/fileio.c:476-484`, well before a later path-based `rename()` at
  `src/fileio.c:187-189`, leaving a TOCTOU window after confirmation/check.
- **Impact:** kg may atomically replace content written by another editor or
  process without warning. This is especially plausible on coarse-timestamp,
  network, and synchronized filesystems; an adversary controlling a writable
  directory can also swap path components between checks and rename.
- **Reproducer/test direction:** open a file, rewrite it to different bytes of
  identical length, restore its recorded mtime with `utimensat()`, then save in
  kg and assert a conflict is reported. Also test unlink/recreate and rename
  replacement between check and final commit with an injectable pre-rename
  hook.
- **Fix direction:** snapshot device, inode, size, and nanosecond mtime (and
  ctime where available) from the opened descriptor. Immediately before
  commit, compare with `fstatat()` relative to an opened parent directory and
  treat lookup failure as a conflict. For a strong invariant, commit only if
  the directory entry still names the accepted inode; use `openat`/`renameat`
  and platform-specific guarded rename where available. Keep “force save” as
  an explicit user decision.

### 5. tiny-regex-c's public matcher is not thread-safe or reentrant

- **Severity:** medium-high for a reusable library
- **Confidence:** high
- **Evidence:** `re_compile()` returns one process-global mutable buffer
  (`fe/tiny-regex-c/re.c:848-857`). Matching uses process-global mutable step
  and depth counters (`fe/tiny-regex-c/re.c:294-298`), reset and modified at
  `fe/tiny-regex-c/re.c:1618-1636` and `fe/tiny-regex-c/re.c:1656-1666`.
- **Impact:** concurrent calls have C data races (undefined behavior), overwrite
  compiled programs returned by other calls, and corrupt each other's
  complexity/depth accounting. A native callback or future threaded kg/Fe
  subsystem cannot safely re-enter the matcher either.
- **Reproducer/test direction:** ThreadSanitizer test with two threads repeatedly
  compiling distinct patterns through `re_compile()` and executing expensive
  matches through `re_exec()`.
- **Fix direction:** deprecate the static-buffer convenience API or make it
  explicitly thread-local; put budgets in a per-execution context passed down
  the matcher. Mark compiled storage immutable after construction. Document
  thread-safety guarantees in `re.h`.

### 6. kg invokes `<ctype.h>` with negative `char` values in hot UTF-8 paths

- **Severity:** medium
- **Confidence:** high
- **Evidence:** `generic_keyword_scan()` passes plain signed `char` values to
  `isspace`, `isprint`, and `isdigit` at `src/syntax.c:1590`,
  `src/syntax.c:1664`, and `src/syntax.c:1736`. The C contract accepts only
  `EOF` or values representable as `unsigned char`; UTF-8 bytes at or above
  0x80 are negative on common signed-char targets. Nearby call sites already
  cast correctly (for example `src/syntax.c:1716`).
- **Impact:** merely opening UTF-8 source in a recognized syntax mode invokes
  undefined behavior. Common glibc implementations mask/index defensively, but
  other libcs may read outside classification tables; locale can also make
  highlighting inconsistent.
- **Reproducer/test direction:** build once with `-fsigned-char`, once with
  `-funsigned-char`, use UTF-8 at leading whitespace/number boundaries in
  every generic syntax, and compare output. A libc interceptor for ctype
  arguments is more useful than UBSan here.
- **Fix direction:** cast every ctype input to `(unsigned char)` and add a CI
  checker/clang-tidy rule for uncast ctype calls. If kg intends locale-free
  ASCII syntax semantics, small ASCII predicates are clearer and deterministic.

### 7. Fe's process and I/O extensions silently truncate data and arguments

- **Severity:** medium (wrong command/data; potential data loss)
- **Confidence:** high
- **Evidence:** `FexExecute` serializes each argument into 1024 bytes and
  ignores the returned/truncation length (`fe/fex_process.c:25-45`); after 31
  arguments it silently stops without rejecting the remaining list
  (`fe/fex_process.c:26-49`). `FexWriteFile` similarly serializes into an
  arbitrary 4 MiB buffer and reports the truncated write as success
  (`fe/fex_io.c:75-90`). `FexOpenFile` and remove/read delimiters also ignore
  truncation (`fe/fex_io.c:40-55`, `fe/fex_io.c:68-72`). The exact byte
  extraction API already exists in `fe/fe.c:799-813`.
- **Impact:** a long pathname can name a different file, long command arguments
  reach `execvp()` altered, extra security-relevant arguments disappear, and
  writing a long string silently loses its tail.
- **Reproducer/test direction:** tests at 1023/1024 bytes, PATH_MAX boundaries,
  31/32 argv entries, and 4 MiB minus/equal/plus one. Have the executed helper
  print argc and argument lengths; verify written files byte-for-byte.
- **Fix direction:** require string/symbol where appropriate; query
  `FeStringByteLength`, checked-add one, allocate exactly, then
  `FeCopyStringBytes`. Reject excess argument counts explicitly (or allocate
  argv dynamically). Loop `fwrite` with clear partial-write/error semantics.

### 8. terminal geometry is trusted without minimum/maximum validation

- **Severity:** medium
- **Confidence:** high
- **Evidence:** cursor-report dimensions are parsed as signed integers without
  validating a positive range (`src/tty.c:525-576`). ioctl dimensions are also
  accepted except for zero columns (`src/tty.c:592-624`). `apply_window_size`
  stores them directly (`src/tty.c:631-643`), and layout can derive zero or
  negative window heights/widths (`src/winmgr.c:105-150`). Many movement and
  rendering calculations assume positive dimensions.
- **Impact:** a 1-row/very narrow pty, a buggy serial terminal, or a spoofed DSR
  response can drive negative geometry, malformed cursor escape sequences,
  incorrect pointer offsets, excessive output/allocation, or crashes.
- **Reproducer/test direction:** PTY cases with 1x1, 2x1, 2x5, and very large
  dimensions; unit-test DSR responses containing negative, zero, overflow,
  and trailing fields.
- **Fix direction:** clamp/reject geometry at one boundary (at least enough for
  echo + mode line + one text cell), cap to a documented operational maximum,
  parse DSR with checked unsigned conversion and exact terminator validation,
  and put assertions at the layout/render boundary.

### 9. FeToString's public zero-size case underflows and writes through `dst`

- **Severity:** medium
- **Confidence:** high
- **Evidence:** `FeToString()` initializes remaining capacity as `size - 1`
  and unconditionally writes a terminator (`fe/fe.c:761-765`). With `size == 0`
  this wraps to `SIZE_MAX`; with a null or zero-byte destination the writer
  performs arbitrary writes. The public declaration has no nonzero-size
  contract (`fe/fe.h:113-116`), and the C API document only says that the
  destination may truncate (`fe/doc/c-api.md:324-326`).
- **Reproducer/test direction:** ASan API test calling `FeToString(ctx, obj,
  NULL, 0)` and another with a one-byte redzone destination and size zero.
- **Fix direction:** define query semantics for size zero (prefer returning the
  required serialization size) or return zero without writing; validate
  `dst != NULL` whenever size is nonzero. Document return/truncation semantics.

### 10. kg's terminal write helper drops short/interrupted writes

- **Severity:** low-medium
- **Confidence:** high
- **Evidence:** `tty_write()` performs exactly one `write()` and discards its
  result (`src/def.h:81-89`). A complete screen frame is passed through it at
  `src/display.c:651-661`.
- **Impact:** SIGWINCH, flow control, a nonblocking/slow pty, or a short write
  can emit half a frame. Besides visible corruption, truncation can strand a
  terminal inside an attribute or OSC/CSI sequence. It is particularly
  relevant once frames grow with splits and Unicode.
- **Reproducer/test direction:** inject a writer returning short counts and
  `EINTR` (as file saving already does) and assert the exact frame is emitted.
- **Fix direction:** use a terminal-specific write-all loop with `size_t`/
  `ssize_t` bounds, retry `EINTR`, and explicitly decide how to handle
  `EAGAIN`, zero writes, and permanent terminal loss.

## Additional concrete issues worth fixing

- `editor_insert_file()` stores `read()` into `size_t` and later casts it back
  to test errors (`src/fileio.c:561-595`). The conversion of `-1` through an
  unsigned type and back is implementation-defined, and capacity arithmetic
  at `src/fileio.c:604-627` is unchecked. Use `ssize_t n`, retry `EINTR`, and
  checked `size_t` growth. **Severity medium, confidence high.**
- Fex `waitpid()` is not retried on `EINTR` (`fe/fex_process.c:53-64`), which
  can leak a zombie and return status `-1`; use the same retry loop kg already
  has. **Severity low-medium, confidence high.**
- Clean EOF from `getdelim()` is reported as an errno tuple built from possibly
  stale `errno` (`fe/fex_io.c:57-65`), commonly `(0, "Success")`. Distinguish
  EOF from `ferror`, and set/check errno deliberately. **Severity low,
  confidence high.**
- `dired_remove()` makes a path-based `lstat()` decision and then a separate
  `rmdir`/`unlink` call (`src/dired.c:494-505`), after a confirmation covering
  only displayed names (`src/dired.c:544-565`). Replacement of a flagged entry
  during that window can delete a different file than the user confirmed.
  Anchor operations to an open directory fd and, where feasible, remember and
  revalidate inode identity. **Severity medium in shared writable directories,
  confidence high.**
- The regex engine's documented recursion ceiling is 4096
  (`fe/tiny-regex-c/re.c:91-101`), but it is an embedded library and makes no
  guarantee that a host thread has a multi-megabyte stack. Convert the matcher
  to an explicit bounded work stack or make the limit derive from caller
  policy; test on small pthread stacks. **Severity medium on embedded/small
  stacks, confidence medium.**

## Recommended invariants and ordering

1. **No untrusted byte reaches a terminal as control syntax.** Fix and
   regression-test this before adding richer presentation or package-driven
   status messages.
2. **Every external resource has one typed owner and one closed state.** Apply
   this first to Fex files, then use the same model for processes and future
   Lisp extension objects.
3. **Public byte-buffer APIs must be alignment-independent or state and check
   alignment.** Resolve this before treating tiny-regex-c as a portable
   reusable component.
4. **A normal save may replace only the disk identity/version the user last
   accepted.** Anything else is a conflict requiring an explicit force.
5. **Lengths remain `size_t`/`ssize_t` until checked conversion at the editor's
   current `int` model boundary.** Centralize checked add/multiply/conversion.
6. **Locale and signedness never change language/syntax correctness.** Audit
   all ctype and number parsing/printing; keep grammar-level ASCII predicates
   locale-independent.
7. **Library state is per context, not process-global.** This is a prerequisite
   for safe richer Fe hosts and any future threaded/reentrant use.

Immediate patch order: terminal escaping; Fex file lifecycle; regex alignment;
save identity/conflict checks; Fex exact string/argv handling; ctype audit;
geometry and partial writes; then regex reentrancy/explicit-stack work.

## Non-findings / defenses that should be preserved

- kg file saving already uses `mkstemp()` in the destination directory, loops
  on short data writes, fsyncs and closes before rename, and removes the
  temporary on failure (`src/fileio.c:126-210`). The missing property is
  guarded target identity/directory durability, not basic tempfile safety.
- Shell region I/O uses concurrent nonblocking pump/read logic, preventing the
  classic “write all stdin before reading stdout” pipe deadlock
  (`src/shell.c:90-193`), and it suppresses SIGPIPE around that pump.
- Compilation and shell child descriptors are marked close-on-exec and child
  failure paths use `_exit`; compilation output strips CSI/OSC sequences before
  inserting it (`src/compile.c:146-200`, `src/compile.c:474-549`).
- SIGWINCH's handler only stores a `sig_atomic_t` flag and preserves errno;
  terminal/layout work occurs later in normal flow (`src/tty.c:719-739`).
- File loading now checks line length and row-array arithmetic before converting
  to the editor's `int` representation (`src/fileio.c:214-257`).
- tiny-regex-c has explicit match step/depth ceilings, checked compiled-buffer
  sizing, bounds-aware character-class compilation, and fuzz/differential
  infrastructure. Those defenses are valuable; the findings above concern
  alignment, global state, and host-stack assumptions around them.
- Fe validates caller arena size and alignment through its public
  `FeMinimumArenaSize`/`FeArenaAlignment` contract and has explicit GC roots.
  The most urgent Fe reliability gap is external-resource ownership in Fex,
  not the core arena layout.
