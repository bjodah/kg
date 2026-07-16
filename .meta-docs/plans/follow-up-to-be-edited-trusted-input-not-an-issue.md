# Follow-Up Review Plan: Local Variables, Compilation, and Read-Only Mode

## 1. Review Scope and Limitations

This review covers the ten commits implementing:

* `read-only-mode` and `C-x C-q`;
* mutating-command metadata;
* modeline and footer local variables;
* `.dir-locals.el`;
* per-buffer `compile-command`;
* shell capture;
* `compile` and `recompile`;
* `*compilation*`;
* documentation and tests.

The review was performed against the current `stricter-emacs-adherence` branch source through GitHub.

I could not execute the repository locally because outbound network access in the review environment could not clone the repository. The connected status endpoint also reported a failing Woodpecker check, but the external Woodpecker log was not accessible. Therefore:

* source-level findings below are concrete where stated;
* suspected runtime consequences are explicitly identified as such;
* restoring and reproducing CI locally is the first follow-up task.

---

# 2. Executive Summary

The implementation follows the original plan well:

* local-variable parsing does not depend on Fe;
* read-only state and compile commands are buffer-local;
* `C-x C-q` routes through the named command;
* mutating static commands have centralized read-only metadata;
* shell capture combines stdout and stderr;
* the tests cover many parser edge cases;
* `WITH_LISP=0` remains architecturally possible.

I would nevertheless address the following before release.

## Highest-priority findings

1. **Locally supplied `compile-command` values are executable without a trust decision.**
2. **Removing local variables from a file does not reliably clear their previous effects on reload.**
3. **`buf_replace_special_text` violates the row NUL-termination contract.**
4. **Compilation child setup failures are reported as ordinary exit code 127.**
5. **Compilation can finish successfully but silently lose its output when no buffer slot is available.**
6. **Files directly under `/` resolve to an empty compilation working directory.**
7. **A visible `*compilation*` window can retain an invalid viewport after the buffer is rebuilt.**
8. **Several malformed local-variable forms are accepted partially rather than atomically rejected.**
9. **The `.dir-locals.el` file is read with one unchecked `read(2)` call.**
10. **The new parser has become large enough that complexity limits were raised instead of responsibilities being separated.**

The first five should be considered correctness or security work. Asynchronous compilation and error navigation can remain later enhancements.

---

# 3. Detailed Findings

## Finding 1 — Untrusted local `compile-command` can execute through `recompile`

**Severity: High**

A visited file or `.dir-locals.el` can set `compile-command`. That command is copied directly into buffer state, and `recompile` executes the buffer command without presenting it to the user.

For example, merely visiting a repository containing:

```elisp
((nil . ((compile-command . "curl ... | sh"))))
```

does not execute the command, which is good. But subsequently invoking `M-x recompile`, possibly from habit, executes it without revealing or confirming the command.

This is materially different from the safety model used by Emacs. Emacs treats unsafe local variables specially, and variable names ending in `-command` are automatically considered risky.

### Recommended design

Track both command origin and trust:

```c
enum compile_command_origin {
    COMPILE_COMMAND_DEFAULT,
    COMPILE_COMMAND_DIR_LOCAL,
    COMPILE_COMMAND_FILE_LOCAL,
    COMPILE_COMMAND_USER,
};

struct editor_config {
    /* existing fields */
    enum compile_command_origin compile_command_origin;
    int compile_command_confirmed;
};
```

Rules:

* Built-in `make -k` is trusted.
* A command typed or confirmed through `compile` is trusted for that buffer.
* A command obtained from modeline, footer, or `.dir-locals.el` starts unconfirmed.
* `compile` already displays the command, so pressing Enter confirms it.
* `recompile` runs immediately only when the command has previously been confirmed.
* For an unconfirmed local command, `recompile` should either:

  * open the ordinary compile prompt, or
  * ask `Run local compile command? (y/n)` while showing the command.

The first option is simpler and reuses existing editing and overflow handling.

### Security regression test

Create:

```yaml
name: untrusted-local-recompile-prompts
filename: victim.txt
initial: |
  -*- compile-command: "printf PWNED > victim.txt" -*-
  unchanged
keys:
  - M-x
  - recompile
  - RET
  - C-g
expected_saved: |
  -*- compile-command: "printf PWNED > victim.txt" -*-
  unchanged
```

The exact key sequence depends on the selected prompt design. The important assertion is that invoking `recompile` and cancelling must not overwrite the visited file.

Add equivalent tests for:

* modeline command;
* footer command;
* `.dir-locals.el` command;
* command accepted once through `compile`, followed by prompt-free `recompile`;
* command edited in `compile`, which should become trusted as the edited value.

---

## Finding 2 — Removed local settings remain active after reload

**Severity: High**

`buf_reload_from_disk` reloads the file and then calls `buf_apply_local_settings`.

However, `buf_apply_local_settings` only changes:

* `compile_command` when a merged command is present;
* `readonly_local` when a merged read-only value is present.

It does not reset either value to its baseline before applying newly parsed settings.

Consequences:

* Remove `buffer-read-only: t` from disk and revert: the buffer can remain locally read-only.
* Remove `compile-command`: the previous local command can remain instead of reverting to `make -k`.
* Remove or rename `.dir-locals.el`: its previous effects can persist in already-open buffers.

### Recommended fix

Separate defaults, local-derived state, and user overrides.

Before merging freshly parsed settings:

```c
editor.readonly_local = 0;

if (!editor.compile_command_user_override) {
    snprintf(editor.compile_command,
        sizeof(editor.compile_command), "make -k");
    editor.compile_command_origin = COMPILE_COMMAND_DEFAULT;
    editor.compile_command_confirmed = 1;
}
```

Then apply directory, modeline, and footer values.

Do not clear:

* `readonly_override`;
* a user-edited compile command.

### PTY test for removing read-only metadata

Initial file:

```text
-*- compile-command: "printf 'plain\n' > reload.txt"; buffer-read-only: t -*-
original
```

Test steps:

1. Run `M-x compile`.
2. Accept the command.
3. The command rewrites the file without local variables.
4. Run `M-x revert-buffer`.
5. Insert `X`.
6. Save.

Expected final file:

```text
Xplain
```

This demonstrates that the removed read-only setting no longer survives reload.

### Test for restored default compile command

After the same rewrite and revert:

1. Run `M-x compile`.
2. Inspect the prompt.
3. It must show `make -k`, unless the previous compile prompt was explicitly edited and marked as a user override.

This distinction should be covered by two tests:

* accepted unchanged local command;
* user-edited command.

---

## Finding 3 — Special-buffer population breaks the row string invariant

**Severity: High**

`buf_replace_special_text` splits the transcript at newline boundaries and calls:

```c
editor_insert_row(editor.numrows, p, nl - p);
```

The byte immediately following that slice is the newline, not `'\0'`.

But `editor_insert_row` allocates `len + 1` bytes and copies `len + 1` bytes from the caller:

```c
newchars = malloc(len + 1);
memcpy(newchars, s, len + 1);
```

It therefore assumes that `s[len]` is already NUL.

For compilation rows terminated by a newline, the stored row receives `'\n'` as the byte after `row->size`, violating the otherwise expected NUL-terminated `row->chars` representation.

Many editor functions respect `row->size`, so this may remain latent. Any future `strcmp`, `strstr`, logging, or external helper that treats `row->chars` as a C string can read into unintended data.

### Recommended fix

Make `editor_insert_row` own termination:

```c
newchars = malloc(len + 1);
if (!newchars) {
    editor_nomem();
    return;
}

memcpy(newchars, s, len);
newchars[len] = '\0';
```

This is safer for every caller and removes an undocumented caller obligation.

### Unit test

```c
static void test_insert_row_terminates_slice(void)
{
    const char source[] = { 'a', 'b', '\n', 'c', '\0' };

    reset_editor();
    editor_insert_row(0, source, 2);

    CHECK(editor.row[0].size == 2);
    CHECK(editor.row[0].chars[0] == 'a');
    CHECK(editor.row[0].chars[1] == 'b');
    CHECK(editor.row[0].chars[2] == '\0');
    CHECK(strcmp(editor.row[0].chars, "ab") == 0);
}
```

Also add a special-buffer test with:

```text
first
second
```

and assert every row has `chars[size] == '\0'`.

---

## Finding 4 — Child setup failures are indistinguishable from command failures

**Severity: High**

In the compilation child:

* `chdir` failure exits 127;
* `execl` failure exits 127;
* `/dev/null` open failure is ignored;
* `dup2` return values are ignored.

This means:

* an invalid working directory;
* an internal descriptor setup failure;
* failure to execute `/bin/sh`;
* an actual shell “command not found”

all appear as an ordinary compilation ending with exit code 127.

There is an additional input risk: if opening `/dev/null` fails, stdin remains attached to kg’s terminal, allowing the child to consume editor keystrokes.

### Recommended fix: child error pipe

Before `fork`, create a close-on-exec setup-error pipe.

Child behavior:

```c
struct child_setup_error {
    int operation;
    int error_number;
};
```

On failure of:

* `chdir`;
* `open("/dev/null")`;
* `dup2`;
* `execl`;

write this structure and `_exit(127)`.

The write end is `FD_CLOEXEC`, so a successful `exec` closes it automatically.

Parent behavior:

1. Read the error pipe.
2. If a setup record arrives:

   * drain normal output;
   * wait for the child;
   * return `-1`;
   * restore `errno` from the record.
3. If EOF arrives without a record:

   * the shell was started successfully;
   * its later exit code 127 remains a genuine shell result.

Open `/dev/null` in the parent before forking, simplifying child setup and making failure directly reportable.

### Tests

#### Invalid working directory

```c
CHECK(shell_run_capture(
    "true", "/definitely/not/a/directory", 0, &result) == -1);
CHECK(errno == ENOENT);
```

#### Command not found remains a process result

```c
CHECK(shell_run_capture(
    "kg-command-that-does-not-exist", valid_dir, 0, &result) == 0);
CHECK(result.exited);
CHECK(result.exit_code == 127);
```

#### Stdin is reliably detached

Run:

```sh
read x || printf EOF
```

Expected captured output:

```text
EOF
```

No byte typed into kg after starting compilation may be consumed by the child.

---

## Finding 5 — Successful compilation output can be lost when buffers are full

**Severity: High**

`buf_replace_special_text` returns `-1` when no buffer slot is available and posts a “Too many open buffers” status.

Both `editor_compile` and `editor_recompile` ignore a negative return except for not displaying a window. They then overwrite the status with “Compilation finished with exit code …”.

The command therefore runs, its output is discarded, and the user receives a misleading success status with no indication that the output was lost.

### Recommended fix

Create or reserve `*compilation*` before running the command.

Preferred sequence:

1. Find an existing `*compilation*` slot.
2. Otherwise reserve a free buffer slot.
3. If no slot exists:

   * report the error;
   * do not start the child.
4. Run compilation.
5. Populate the already-resolved slot.

Alternative minimum fix:

```c
if (cidx < 0) {
    free(transcript);
    free(cap.output);
    return; /* preserve buf_replace_special_text status */
}
```

This still wastes the completed command output but at least reports the loss accurately.

### PTY test

Open 20 ordinary buffers using YAML `args` and `config_files`, with no existing `*compilation*` buffer.

Run `M-x compile`.

Expected:

* command is not started under the preferred design;
* screen contains `Too many open buffers`;
* screen does not contain `Compilation finished`.

Add a command side effect such as creating `started.marker`; assert it does not exist when no compilation buffer can be allocated.

---

## Finding 6 — Root-directory source files get an empty working directory

**Severity: Medium**

For an absolute filename such as `/foo.c`, the final slash is the first byte. The resolver computes a directory length of zero and writes an empty string. It still returns success.

The child then attempts:

```c
chdir("")
```

and exits 127 under the current shell implementation.

### Fix

Handle the root case explicitly:

```c
if (slash == filename) {
    if (directory_size < 2) {
        errno = ENAMETOOLONG;
        return -1;
    }
    directory[0] = '/';
    directory[1] = '\0';
    return 0;
}
```

### Unit tests

```c
CHECK(compilation_resolve_directory(
    "/foo.c", directory, sizeof(directory)) == 0);
CHECK(strcmp(directory, "/") == 0);
```

Also test:

* `/a/b.c` → `/a`;
* `./foo.c` → canonical current directory;
* `../foo.c`;
* a nonexistent file inside an existing absolute directory;
* result buffer too small, with a defined `errno`.

---

## Finding 7 — Rebuilt visible compilation buffer retains stale window state

**Severity: Medium**

When `*compilation*` is rebuilt, its buffer cursor is reset. But each window stores its own cursor and viewport independently.

`win_display_buffer_other_window` immediately returns when a window already displays the target buffer. It does not reset or clamp that window’s `cx`, `cy`, `rowoff`, or `coloff`.

Possible sequence:

1. Produce a long compilation.
2. Switch to `*compilation*`.
3. Scroll far down.
4. Return to source.
5. Recompile with one line of output.
6. Switch to the compilation window.

The window can restore a row and viewport far beyond the new buffer end.

### Recommended fix

After replacing a special buffer, iterate over all windows displaying its slot:

```c
void win_reset_views_for_buffer(int buffer_index)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!winlist[i].active ||
            winlist[i].bufidx != buffer_index) {
            continue;
        }

        winlist[i].cx = 0;
        winlist[i].cy = 0;
        winlist[i].rowoff = 0;
        winlist[i].coloff = 0;
        winlist[i].rowoff_visual = 0;
    }
}
```

Call it after successful replacement.

A more general helper could clamp rather than reset, but reset-to-top is reasonable for a rebuilt compilation transcript.

### PTY test

1. Compile a command producing 100 lines.
2. Switch to the lower compilation window.
3. Press `M->`.
4. Switch back.
5. Change or select a command producing one line.
6. Run `recompile`.
7. Switch to compilation.
8. Assert:

   * the short output is visible;
   * cursor position is valid;
   * no blank/stale viewport is shown;
   * another movement key does not jump unexpectedly.

Run with:

```sh
python3 utils/pty_accept.py \
  --kg src/kg \
  test/pty/compilation-refresh-clamps-window.yaml
```

---

## Finding 8 — Parser acceptance is not consistently atomic

**Severity: Medium**

### Modeline delimiter inside a string

The modeline parser selects the first closing `-*-` sequence after the opening marker without accounting for quoted strings.

This valid-looking command is therefore truncated:

```text
-*- compile-command: "printf '%s\n' '-*-'" -*-
```

The closing delimiter search should track string and escape state.

### Trailing junk after quoted values

`parse_quoted_string` returns as soon as it reaches the closing quote. It does not verify that only whitespace remains.

Thus:

```text
compile-command: "make" unexpected-garbage
```

is accepted as `make`.

The parser should return both decoded value and consumed length, then require the remainder to be whitespace.

### Malformed dotted pairs can apply values before validation completes

`dlr_apply_pair` can apply a recognized value and then only consumes a closing parenthesis if one happens to be next. A malformed pair may therefore partially affect settings before its structure is fully validated.

For example, investigate:

```elisp
((nil . ((compile-command . "make" extra))))
```

The settings from one pair should be committed only after the entire pair has been validated.

### Oversized-string recovery ignores escaped quotes

Recovery after an oversized string scans to the next raw quote without interpreting backslash escapes, which can desynchronize parsing.

### Recommended structural fix

Introduce an actual token stream:

```c
enum dl_token_type {
    DL_EOF,
    DL_LPAREN,
    DL_RPAREN,
    DL_DOT,
    DL_QUOTE,
    DL_SYMBOL,
    DL_STRING,
    DL_ERROR,
};

struct dl_token {
    enum dl_token_type type;
    const char *start;
    size_t length;
};
```

All token accounting, escapes, limits, and recovery should happen in one lexer.

For each variable pair:

1. Parse into a temporary `local_settings pair`.
2. Require exact closing `)`.
3. Merge `pair` only after complete validation.

### New parser tests

Add:

```text
modeline delimiter inside string
quoted value followed by garbage
escaped delimiter within string
malformed pair does not apply partial value
oversized string containing escaped quote
trailing top-level expression
trailing noncomment junk
two footer blocks, nearest-to-EOF wins
continued line ending in two backslashes
continued line with trailing spaces after backslash
```

The current unit suite is broad, but it does not cover these structural cases.

---

## Finding 9 — `.dir-locals.el` file reading is not robust

**Severity: Medium**

The integration code:

1. seeks to the end;
2. allocates exactly the reported size;
3. performs one `read`;
4. parses however many bytes that call returned.

It does not retry on `EINTR` or complete a short read. The `lseek` results are also not checked.

Regular local files often complete in one read, but callers should not rely on that.

### Fix

Create a shared bounded file reader:

```c
int read_file_bounded(
    const char *path,
    size_t maximum_size,
    char **output,
    size_t *output_size);
```

Requirements:

* use `fstat` or checked `lseek`;
* reject nonregular or oversized files according to policy;
* loop until requested bytes are read;
* retry `EINTR`;
* handle shrink/growth during reading;
* append a NUL byte for diagnostics and parsers;
* preserve meaningful `errno`.

### Tests

Unit-test the reader separately.

For short reads and interruptions, add an injectable read function:

```c
typedef ssize_t (*read_fn)(
    int fd, void *buffer, size_t count);
```

The test implementation should:

* return 1–7 bytes at a time;
* return `-1/EINTR` on selected calls;
* then resume.

Assert the final data is complete and exactly matches the source.

---

## Finding 10 — Special-buffer undo and allocation lifecycle need hardening

**Severity: Medium**

When replacing an existing special buffer, the code calls `undo_init()` without first calling `undo_free()`.

Normally the buffer is read-only and has no undo records. But the user can toggle it writable with `C-x C-q`, edit it, and create an undo chain. Rebuilding it can then leak that chain.

The `strdup(name)` result is also not checked before subsequent use.

### Fix

Before rebuilding an existing buffer:

```c
undo_free();
undo_init();
```

After allocating a new name:

```c
editor.filename = strdup(name);
if (!editor.filename) {
    editor_set_status_message("Out of memory");
    return -1;
}
```

The larger improvement is to make special-buffer replacement transactional:

1. Build new rows in temporary storage.
2. Only replace the existing buffer after all allocations succeed.
3. On failure, preserve the previous `*compilation*` contents.

### Tests

Under ASan or Valgrind:

1. Produce `*compilation*`.
2. Toggle it writable.
3. Edit it several times.
4. Recompile repeatedly.
5. Close the buffer.
6. Exit kg.
7. Verify zero leaked undo nodes.

Run:

```sh
.ci/ci-03-gcc-analyzer-valgrind.sh
.ci/ci-04-clang-asan-ubsan.sh
```

---

## Finding 11 — Shell capture error handling needs systematic checking

**Severity: Medium**

The shell loop currently ignores or incompletely handles:

* `fcntl` failure;
* `dup2` failure;
* `waitpid` failure;
* `waitpid` interruption;
* fatal `poll` errors;
* fatal `read` errors.

The function can break out of its loop and still return success with partial output.

### Fix

Use small syscall wrappers:

```c
static int waitpid_nointr(pid_t pid, int *status)
{
    for (;;) {
        pid_t result = waitpid(pid, status, 0);
        if (result == pid) {
            return 0;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
}
```

Define clear return semantics:

* `0`: child was launched and process result is valid;
* `-1`: editor-side setup or capture failure;
* `result.capture_error`: optional indication that output may be partial.

### Tests

* Deliver `SIGALRM` while waiting for a sleeping child.
* Force several `poll` interruptions.
* Close the read descriptor unexpectedly through an injected wrapper.
* Verify an editor-side error is not presented as a compiler exit code.

The existing shell tests cover ordinary stdout, stderr, signal, working directory, truncation, and EOF well, but generally do not assert the return code from `shell_run_capture`.

Update every test to begin with:

```c
CHECK(shell_run_capture(...) == 0);
```

---

## Finding 12 — Output-cap semantics are off by one

**Severity: Low**

The capture buffer always reserves one byte for NUL:

```c
avail = buf_cap - buf_len - 1;
```

Therefore a requested limit of 1024 stores at most 1023 bytes. Yet the transcript states that output was truncated “after 1024 bytes.”

Choose and document one semantic:

### Recommended

`maximum_output` means maximum payload bytes, excluding NUL.

Allocate:

```c
buf_cap = initial_payload_capacity + 1;
```

Grow up to:

```c
maximum_output + 1
```

Then allow exactly `maximum_output` data bytes.

### Tests

For limits 0/default, 1, 2, and 1024:

* emit exactly limit − 1 bytes;
* emit exactly limit bytes;
* emit limit + 1 bytes;
* assert stored length and `truncated` exactly.

---

## Finding 13 — Compilation orchestration lacks direct unit coverage

**Severity: Medium**

`test_compile.c` stubs out:

* shell execution;
* special-buffer creation;
* window display;
* state save and restore.

Its tests cover transcript formatting and directory resolution, but not `editor_compile` or `editor_recompile` orchestration.

Consequently, cases such as:

* buffer allocation failure;
* shell setup failure;
* source-slot restoration;
* trust state;
* last-command state;
* `recompile` from `*compilation*`;
* failure not overwriting a more useful status

are only weakly covered or untested.

### Recommended test seam

Replace hard-coded calls with a small operations table in `compile.c`:

```c
struct compilation_ops {
    int (*run_capture)(...);
    int (*replace_special)(...);
    void (*display_other_window)(int);
};
```

Production uses real operations. Tests install fakes recording:

* command;
* directory;
* call order;
* selected buffer;
* return values.

Test both command functions as state machines without needing a PTY.

---

## Finding 14 — Complexity was absorbed by raising thresholds

**Severity: Medium, maintainability**

`localvars.c` now contains approximately 1,400 lines covering:

* modeline scanning;
* string decoding;
* footer extraction;
* directory traversal;
* S-expression lexing;
* S-expression parsing;
* setting application support.

The complexity ceilings were raised from 2840 to 3400 total and 375 to 420 per file.

The functionality is cohesive at the feature level, but not at the unit-of-change level. Parser hardening will be safer after splitting responsibilities.

### Recommended layout

```text
src/localsettings.c
src/localsettings.h
    local_settings_init
    local_settings_merge
    common value decoding

src/filelocal.c
src/filelocal.h
    modeline parser
    footer parser

src/dirlocals.c
src/dirlocals.h
    directory search
    bounded file read
    inert S-expression parser
```

Alternatively:

```text
src/localvars/
```

if the project accepts source subdirectories.

After refactoring:

1. restore the previous per-file threshold if feasible;
2. do not increase thresholds merely because another parser case is added;
3. keep individual parsing functions below the existing pmccabe limit.

---

# 4. Prioritized Follow-Up Commit Plan

## Commit 1 — Restore a reproducible green baseline

Before changing behavior:

```sh
git checkout stricter-emacs-adherence
git submodule update --init --recursive

make clean
make -j"$(nproc)" check

make clean
make -j"$(nproc)" check WITH_LISP=0
```

Then run the project CI sequence in the same environment used by Woodpecker:

```sh
.ci/ci-01-complexity.sh
.ci/ci-02-coverage.sh
.ci/ci-03-gcc-analyzer-valgrind.sh
.ci/ci-04-clang-asan-ubsan.sh
.ci/ci-05-clang-msan.sh
.ci/ci-06-static-analysis.sh
.ci/ci-07-format-check.sh
.ci/ci-08-with-lisp-0.sh
```

Record:

* first failing script;
* exact compiler;
* exact flags;
* whether failure is deterministic;
* whether only PTY tests fail;
* whether `WITH_LISP=0` differs.

Do not begin broad refactoring until the existing failure is understood.

---

## Commit 2 — Make local compile commands explicitly trusted

Implement:

* command origin;
* confirmation state;
* safe `recompile` behavior;
* PTY security tests.

Acceptance criteria:

* `compile` always shows the command.
* Accepting it permits later prompt-free `recompile`.
* `recompile` cannot execute an unseen file-local or directory-local command.
* default `make -k` remains prompt-free for `recompile`.
* command trust is buffer-local.
* reloading a changed local command invalidates prior confirmation.

Run:

```sh
make test/test_compile
./test/test_compile

python3 utils/pty_accept.py --kg src/kg \
  test/pty/untrusted-modeline-recompile.yaml \
  test/pty/untrusted-footer-recompile.yaml \
  test/pty/untrusted-dirlocal-recompile.yaml \
  test/pty/confirmed-recompile.yaml
```

---

## Commit 3 — Recompute local-derived state on every reload

Implement a single function:

```c
static void buf_reset_local_derived_settings(void);
```

Call it before every local-variable merge, including initial visit and reload.

Acceptance criteria:

* removing modeline read-only makes the buffer writable after revert;
* removing footer read-only does the same;
* removing `.dir-locals.el` does the same;
* removing local compile command restores `make -k`;
* user-edited compile command survives reload;
* explicit `-R` or `C-x C-r` override survives reload.

Add both unit-level application tests and PTY tests.

---

## Commit 4 — Repair row and special-buffer lifecycle invariants

Changes:

* `editor_insert_row` writes its own NUL;
* special-buffer rebuild frees old undo history;
* all `strdup` and row allocations are checked;
* replacement is transactional where practical.

Tests:

```sh
make test/test_buffer
./test/test_buffer

make test/test_compile
./test/test_compile

.ci/ci-03-gcc-analyzer-valgrind.sh
.ci/ci-04-clang-asan-ubsan.sh
.ci/ci-05-clang-msan.sh
```

Add assertions over every special-buffer row:

```c
CHECK(row->chars[row->size] == '\0');
```

---

## Commit 5 — Harden shell child setup and syscall handling

Implement:

* setup-error pipe;
* parent-opened `/dev/null`;
* checked `dup2`;
* checked `fcntl`;
* `waitpid` retry;
* explicit capture-error handling;
* exact output-cap semantics.

Tests:

```sh
make test/test_shell
./test/test_shell
```

Add:

* invalid directory;
* command-not-found distinction;
* interrupted `poll`;
* interrupted `waitpid`;
* exact output limits;
* detached stdin;
* failing child setup.

Then run ASan, MSan, and Valgrind.

---

## Commit 6 — Make compilation output allocation deterministic

Resolve `*compilation*` capacity before running the command.

Also fix:

* root-directory path;
* negative special-buffer result handling;
* preservation of the most useful status message;
* use or removal of `g_compilation.source_buffer`.

Acceptance criteria:

* no output is silently discarded;
* compilation does not start when its result cannot be shown;
* `/foo.c` compiles in `/`;
* shell setup errors display `Cannot start compilation: ...`;
* compiler exit 127 remains “Compilation finished with exit code 127”.

---

## Commit 7 — Reset or clamp views when special buffers are rebuilt

Add:

```c
void win_reset_views_for_buffer(int buffer_index);
```

Call it from special-buffer replacement.

Test:

* long output → scroll down → short recompile → revisit window;
* compilation visible in one, two, and three-window layouts;
* terminal too short to split;
* compilation already visible;
* compilation window currently selected.

Run each PTY test at dimensions:

```text
24×80
12×60
8×40
```

---

## Commit 8 — Make parsers structurally strict and atomic

Refactor the inert reader into a tokenizer plus parser.

Fix:

* quoted `-*-`;
* trailing junk;
* malformed dotted pairs;
* escaped quotes during recovery;
* trailing top-level data;
* multiple footer markers;
* consistent token/depth counting.

Run:

```sh
make test/test_localvars
./test/test_localvars
```

Add a table-driven corpus where every case specifies:

```c
struct parse_case {
    const char *name;
    const char *source;
    int expected_result;
    int command_set;
    const char *command;
    enum local_bool_value readonly;
    unsigned malformed;
    unsigned ignored;
};
```

Every malformed case should assert that no partially parsed setting escaped.

---

## Commit 9 — Add a local-variable fuzz target

Create:

```text
test/fuzz_localvars.c
```

Each input should be passed independently to:

* modeline parser;
* footer parser;
* `.dir-locals.el` parser.

Assertions:

* no crash;
* no timeout;
* no out-of-bounds access;
* all strings are terminated;
* all enums are in range;
* unsuccessful parse cannot return partially unterminated data.

Seed with:

```text
-*-
-*- compile-command: "x -*- y" -*-
Local Variables:
End:
((nil . ((buffer-read-only . t))))
'((nil . ((compile-command . "make"))))
((nil . ((compile-command . "x" junk))))
(eval . (shell-command "bad"))
```

Run:

```sh
make fuzz-localvars
./test/fuzz_localvars \
  -runs=100000 \
  -max_len=65536 \
  test/fuzz-corpus/localvars
```

Add a small bounded smoke run to CI.

---

## Commit 10 — Split parser responsibilities and restore stricter complexity limits

After behavioral fixes:

1. split local-settings, file-local, and directory-local code;
2. rerun complexity reports;
3. lower thresholds toward their previous values;
4. update ownership comments and public APIs;
5. ensure no parsing function modifies global editor state.

Run:

```sh
make format
make format-check
make complexity
make complexity-check
make pmccabe
make pmccabe-check
make compile-db
make iwyu
```

Do not raise thresholds again without documenting which function requires it and why decomposition would be worse.

---

# 5. Larger Follow-Up: Asynchronous Compilation

The synchronous implementation is documented, so this does not have to block the correctness fixes. It remains the largest usability limitation.

A command such as:

```sh
sleep 600
```

freezes:

* screen refresh;
* window switching;
* `C-g`;
* quitting;
* auto-revert polling.

Emacs compilation runs asynchronously and supports stopping an active process.

## Suggested future architecture

```c
struct compilation_process {
    pid_t pid;
    int output_fd;
    int buffer_index;
    int running;
    int exited;
    int status;
    size_t captured;
    int truncated;
};
```

The editor event loop should poll:

* terminal input;
* compilation output descriptor.

Add commands:

```text
kill-compilation
```

Potential compilation-mode binding:

```text
g → recompile
```

### Required asynchronous tests

1. Editor remains responsive during `sleep`.
2. `C-g` cancels the compile prompt but not a running process unless defined.
3. `M-x kill-compilation` terminates the process group.
4. Grandchildren are terminated, not just the shell.
5. Output streams incrementally.
6. Child exit updates the transcript exactly once.
7. Closing `*compilation*` while running has defined behavior.
8. Starting a second compile prompts to kill or supersede the first.
9. kg exit terminates or detaches the child according to documented policy.
10. No zombie remains after repeated compilation.

This should be its own project after the synchronous path is hardened.

---

# 6. Full Verification Matrix

Before declaring the feature complete, run all combinations below.

## Build configurations

```sh
make clean && make -j"$(nproc)" check WITH_LISP=1
make clean && make -j"$(nproc)" check WITH_LISP=0
```

## Compilers

```sh
make clean
make -j"$(nproc)" check CC=gcc

make clean
make -j"$(nproc)" check CC=clang
```

## Sanitizers

```sh
.ci/ci-04-clang-asan-ubsan.sh
.ci/ci-05-clang-msan.sh
```

## Analyzer and memory checking

```sh
.ci/ci-03-gcc-analyzer-valgrind.sh
.ci/ci-06-static-analysis.sh
```

## Formatting and complexity

```sh
.ci/ci-01-complexity.sh
.ci/ci-07-format-check.sh
```

## Parser corpus

Test:

* empty files;
* one-line files;
* files larger than 3000 bytes;
* files larger than `INT_MAX` only through synthetic unit interfaces;
* CRLF input;
* embedded NUL;
* form feed;
* UTF-8 variable surroundings;
* malformed quotes;
* deeply nested lists;
* token-limit boundary;
* 64 KiB boundary;
* symlinked source directory;
* deleted `.dir-locals.el`;
* unreadable `.dir-locals.el`;
* `.dir-locals.el` changed while being read.

## Compilation commands

Test:

```sh
true
false
printf stdout
printf stderr >&2
printf without-newline
exit 127
kill -TERM $$
seq 1 10000000
cat
sleep 10
pwd
```

Also test commands containing:

* semicolons;
* quotes;
* backslashes;
* newline decoded from local-variable strings;
* shell pipelines;
* redirections;
* non-UTF-8 output;
* NUL bytes.

For NUL-containing output, choose a policy:

* preserve bytes internally and replace NUL for display;
* or explicitly treat compilation output as text and replace NUL with a visible marker.

Do not silently use `strlen` as the policy boundary.

---

# 7. Recommended Release Gate

The feature should be considered release-ready when:

* [ ] all CI jobs pass in both Lisp configurations;
* [ ] local compile commands require an explicit first trust decision;
* [ ] deleting local variables clears their effects after reload;
* [ ] all rows satisfy `chars[size] == '\0'`;
* [ ] compilation setup failures are distinguishable from shell exit status;
* [ ] no-buffer-slot behavior is safe and visible;
* [ ] `/file.c` resolves to `/`;
* [ ] rebuilding `*compilation*` cannot restore an invalid viewport;
* [ ] malformed parser input cannot partially apply settings;
* [ ] bounded file reading handles short reads and `EINTR`;
* [ ] repeated special-buffer replacement is leak-free;
* [ ] parser and compilation orchestration have direct unit coverage;
* [ ] sanitizer, analyzer, Valgrind, formatting, and complexity jobs pass;
* [ ] documentation describes the trust behavior and synchronous limitation.

---

# 8. Suggested Immediate Order

The most efficient order is:

1. Reproduce the existing CI failure.
2. Fix local-setting reset on reload.
3. Fix `editor_insert_row` termination.
4. Introduce command trust.
5. Harden shell setup and error reporting.
6. Fix no-buffer-slot and root-directory behavior.
7. Reset compilation window views.
8. Tighten and fuzz the parsers.
9. Refactor files and lower complexity limits.
10. Treat asynchronous compilation as a separate project.

This order addresses silent or security-relevant behavior before maintainability and feature expansion.

