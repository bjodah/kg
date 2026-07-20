## Recommendation

Implement this as **single-threaded, event-loop-driven asynchronous compilation**:

1. Spawn `/bin/sh -c <compile-command>` and return immediately.
2. Create/reset `*compilation*` at once.
3. Display it in a horizontal split while retaining focus in the source window.
4. Drain combined stdout/stderr incrementally from a nonblocking pipe.
5. Append output to the read-only buffer in bounded chunks.
6. Reap the child with `waitpid(..., WNOHANG)`.
7. Add `kill-compilation`.
8. Treat ANSI support as a second, separable layer over the streaming implementation.

I would **not** add threads. The editor is currently built around mutable global editor/buffer/window state; threads would introduce locking and redraw synchronization for no real benefit. The terminal already wakes every 100 ms, which is sufficient for cooperative background I/O.

---

# Decisions for you

Most choices have safe defaults. Two materially affect scope.

| Decision                                    | Recommended default                                                          | Why                                                                             |
| ------------------------------------------- | ---------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| Starting a compilation while one is running | Prompt to terminate the old compilation                                      | Avoid concurrent writers to one `*compilation*` buffer                          |
| ANSI support in the first implementation    | Phase it: clean streaming first, ANSI rendering second                       | ANSI requires a display-attribute extension, not merely stripping escape bytes  |
| How output gets colors                      | Interpret ANSI only when the child emits it                                  | Pipes do not cause many tools to enable color automatically                     |
| Pipe versus pseudo-terminal                 | Pipe                                                                         | A PTY adds terminal emulation, interactive-input and process-control complexity |
| Output window behavior                      | Reuse a window already showing `*compilation*`; otherwise split horizontally | Avoid creating a new window on every recompile                                  |
| Focus after starting compilation            | Keep focus in the source window                                              | Matches the current other-window behavior and permits immediate editing         |
| Auto-scroll                                 | Follow output only while the window was already at the end                   | Users can scroll upward without the process continually snapping them back      |
| Output limit                                | Preserve the existing 8 MiB limit                                            | Prevent unbounded editor memory consumption                                     |
| Cancellation command                        | Add `M-x kill-compilation`, plus `C-c C-k` in `*compilation*`                | Long-running or stuck builds otherwise cannot be controlled cleanly             |
| Multiple simultaneous compilations          | One global compilation for now                                               | Current state and buffer naming already imply one compilation session           |

The only choices I would actively request before implementation are:

1. **Should ANSI rendering be part of the same change, or a follow-up?** I recommend a follow-up commit or PR.
2. **When compilation is already running, should `compile` prompt before terminating it, or always terminate and restart?** I recommend prompting; `recompile` could optionally restart without prompting.

---

# Current behavior and root cause

The blocking is very localized.

`editor_compile()` currently:

1. Reads the command.
2. Resolves the working directory.
3. Calls `shell_run_capture()`.
4. Waits for all output and process termination.
5. Constructs the complete transcript.
6. Only then creates and displays `*compilation*`.

`editor_recompile()` repeats the same synchronous flow.

Inside `shell_run_capture()`, the parent blocks in:

```c
poll(&pfd, 1, -1);
```

until data arrives or the pipe closes, and finally performs blocking `waitpid(pid, &status, 0)`.

That means the editor’s main loop cannot process terminal input, redraw windows, handle resize, or auto-revert until the compilation finishes.

The current completed-output path already gives us several useful pieces to preserve:

* Combined stdout and stderr.
* `/bin/sh -c`.
* Per-buffer compile command.
* Source-file directory as working directory.
* A single `*compilation*` buffer.
* Read-only special-buffer presentation.
* Horizontal other-window display.
* Last-command and last-directory state for `recompile`.
* The 8 MiB output cap.

---

# Target user-visible behavior

A completed implementation should behave as follows.

## Starting compilation

After `M-x compile` and Enter:

* The command prompt disappears immediately after the child has been spawned.
* A `*compilation*` buffer is created or cleared.
* A header appears immediately:

```text
Compilation started in /path/to/project

$ make
```

* The buffer is displayed in another window.
* Focus remains in the original source window.
* The status area says something such as:

```text
Compilation started: make
```

The existing window helper already has nearly the desired policy: with one window it creates a horizontal split, and with multiple windows it reuses an available other window. It does not select the compilation window.

## While running

* Output appears incrementally.
* Keyboard input, editing, saving, switching buffers and changing windows remain responsive.
* Terminal resize continues to work.
* stdout and stderr stay in their naturally interleaved order by sharing one pipe.
* The output window follows the tail unless the user has scrolled away.
* The process continues to be drained while the user is sitting in an `M-x`, filename or confirmation prompt, even if the compilation window is not redrawn during that prompt.

## On completion

Append one of:

```text
Compilation finished with exit code 0
```

or:

```text
Compilation terminated by signal 15
```

Then:

* Close the output descriptor.
* Record the child status.
* Clear the active compilation state.
* Update the status area.
* Leave the completed buffer read-only and available for inspection.

## On cancellation

`M-x kill-compilation` should:

* Send an interrupt to the compilation process group.
* Continue draining any final output.
* Append a termination message once reaped.
* Avoid leaving child or grandchild processes behind.
* Avoid zombies.

---

# Detailed architecture

## 1. Replace the transcript-oriented state with a process lifecycle

Extend `struct compilation_state` in `src/compile.h`.

A suitable state shape would be:

```c
enum compilation_phase {
    COMPILATION_IDLE,
    COMPILATION_RUNNING,
    COMPILATION_TERMINATING,
};

struct compilation_state {
    enum compilation_phase phase;

    bool have_last_command;
    char last_command[KG_COMPILE_COMMAND_MAX];
    char last_directory[PATH_MAX];

    int source_buffer;
    int compilation_buffer;

    pid_t pid;
    pid_t process_group;
    int output_fd;

    bool pipe_eof;
    bool child_reaped;
    int wait_status;

    size_t stored_output;
    size_t maximum_output;
    bool truncated;
    bool truncation_marker_written;

    char pending_line[/* dynamically managed instead */];
    size_t pending_line_length;

    /* Needed only when ANSI handling is enabled. */
    struct compilation_ansi_state ansi;
};
```

Use dynamic storage for the partial line rather than a fixed array, because compiler diagnostics can contain very long lines.

The important distinction is:

* **Pipe EOF** means no more bytes will arrive.
* **Child reaped** means exit status is known.

Do not finalize until both are true. A child can exit while unread bytes remain in the pipe, and pipe EOF can become visible just before `waitpid(..., WNOHANG)` reports the child.

## 2. Introduce a small state machine

The process lifecycle should be explicit.

### `COMPILATION_IDLE`

No child and no open output descriptor.

### `COMPILATION_RUNNING`

The child exists or still needs reaping. The output descriptor may still be readable.

### `COMPILATION_TERMINATING`

An interrupt or termination signal was sent. Continue draining and polling exactly as in the running state.

A separate `DRAINING` state is probably unnecessary if `pipe_eof` and `child_reaped` are tracked independently.

---

# Process spawning

Add a compile-specific asynchronous spawn helper in `src/compile.c`:

```c
static int compilation_spawn(
    const char *command,
    const char *directory,
    pid_t *pid_out,
    int *output_fd_out);
```

Keeping it compile-specific is consistent with the repository’s preference for small, local changes and no new dependencies.

## Parent setup

1. Create a pipe.
2. Mark both ends close-on-exec.
3. `fork()`.
4. Close the write end in the parent.
5. Make the read end nonblocking.
6. Store the PID and descriptor.
7. Best-effort `setpgid(pid, pid)` in the parent to close the race with the child.
8. Return to the editor immediately.

## Child setup

1. Call `setpgid(0, 0)` so the shell becomes process-group leader.
2. `chdir(directory)`.
3. Connect stdin to `/dev/null`.
4. Connect both stdout and stderr to the pipe’s write end.
5. Close inherited pipe descriptors.
6. Execute:

```c
execl("/bin/sh", "sh", "-c", command, (char *)NULL);
```

7. Exit with 127 on setup or exec failure.

A process group is important. Killing only the shell PID may leave `make`, compiler processes, test runners or pipelines alive.

## Avoid a SIGCHLD handler initially

Do not add asynchronous signal-handler logic. Polling:

```c
waitpid(pid, &status, WNOHANG)
```

from the existing main thread is simpler and safer.

A SIGCHLD handler would need a self-pipe or another signal-safe handoff mechanism before editor state could be touched safely.

---

# Background polling

Add:

```c
int compilation_poll(void);
```

Return nonzero when visible state changed and a redraw would be useful.

Each invocation should:

1. Read available bytes from the nonblocking pipe.
2. Stop after a fixed work budget.
3. Append accepted output to the compilation buffer.
4. Drain but discard bytes after the storage cap.
5. Check `waitpid(..., WNOHANG)`.
6. Finalize when both EOF and child status are available.

## Work budget

Do not drain arbitrarily large output in one editor tick. Otherwise a compiler that continuously writes can still starve keyboard processing.

A reasonable initial budget:

```c
#define COMPILATION_READ_CHUNK 4096
#define COMPILATION_TICK_BUDGET (64 * 1024)
```

On each poll:

* Read up to 64 KiB.
* Return to the editor.
* Drain more on the next loop iteration.

This gives much better fairness than “read until EAGAIN” when the producer is faster than the terminal redraw path.

## Main-loop integration

The top-level loop should poll before drawing:

```c
while (running) {
    editor_process_pending_signals();
    compilation_poll();
    autorevert_poll();

    if (!editor_input_flood(STDIN_FILENO)) {
        editor_refresh_screen();
    }
    editor_process_keypress(STDIN_FILENO);
}
```

However, this alone is insufficient. `editor_process_keypress()` enters `editor_read_key_idle()`, which waits internally until a key arrives. The main loop may therefore remain inside that function for minutes.

`editor_read_key_idle()` already receives a timeout every 100 ms and calls auto-revert. Add compilation polling there:

```c
if (nread == 0) {
    int changed = 0;

    changed |= autorevert_poll();
    changed |= compilation_poll();

    if (changed) {
        editor_refresh_screen();
    }
    continue;
}
```

The 100 ms wakeup comes from the terminal’s `VTIME = 1` configuration.

## Poll while minibuffers are active

The plain `editor_read_key()` is used by minibuffers and yes/no prompts. It should also call `compilation_poll()` on timeout, but should generally **not redraw the entire screen** there.

That gives:

* Continuous pipe drainage.
* No child blockage due to a full pipe.
* No minibuffer prompt corruption.
* Compilation output becomes visible when the prompt next redraws or exits.

I would factor this into something like:

```c
static int editor_background_poll(bool allow_refresh);
```

or simply call `compilation_poll()` from each timed input loop and let the caller decide whether to refresh.

Audit at least:

* `editor_read_key_idle()`
* `editor_read_key()`
* `editor_read_raw_byte()`
* Any confirmation loop that reads directly
* Cursor-position/query loops that can wait repeatedly

---

# Streaming into an inactive buffer

This is the most important non-process refactor.

The current `buf_replace_special_text()`:

* Saves the current buffer.
* Switches global editor state into the target buffer.
* Deletes every row.
* Rebuilds the complete contents.
* Saves the target.
* Potentially restores another buffer.

Calling that for every output chunk would be:

* O(total output²) over a long build.
* Visually disruptive.
* Fragile when the compilation buffer is displayed in an inactive window.
* Incompatible with preserving the user’s scroll position.

## Add explicit special-buffer APIs

I would split creation/reset from append:

```c
int buf_prepare_special_text(
    const char *name,
    struct editor_syntax *syntax,
    int readonly);

int buf_append_special_text(
    int buffer_index,
    const char *text,
    size_t text_length);

void buf_clear_special_text(int buffer_index);
```

`buf_replace_special_text()` can remain as a convenience wrapper implemented using prepare, clear and append.

## Do not call `buf_restore_from_slot()` merely to append

`buf_restore_from_slot()` also updates the active window’s buffer association.

That is appropriate for an actual buffer switch, but not for background mutation.

## Recommended internal mechanism

The existing row functions operate through global `editor.row`, `editor.numrows` and `editor.syntax`. `editor_insert_row()` and `editor_update_row()` depend on that global context.

For the smallest safe refactor, add an internal temporary row binding in `bufmgr.c`:

1. Save only the live row-related editor fields:

   * `editor.row`
   * `editor.numrows`
   * `editor.dirty`
   * `editor.syntax`
2. Bind those fields to the target `editor_buffer`.
3. Perform row append operations.
4. Copy the updated pointers and counts back to the target slot.
5. Restore the live fields.
6. Never call a window-switching function.

Because everything remains on the editor’s main thread, this does not create concurrency hazards.

A longer-term cleaner design would make row operations accept an explicit buffer object, but that is a substantially larger refactor than this feature needs.

## Incremental line handling

Process output chunks will not align with line boundaries.

Maintain a pending logical line:

* Append ordinary bytes to the pending line.
* On `\n`, append or complete one editor row.
* Keep any remaining bytes for the next read.
* On EOF, flush the final unterminated line.
* Ensure the finish message begins on a fresh line.

Appending a complete row is cheap. Updating the final existing row on every byte is not. Process data in chunks and mutate the last row at most once per read batch.

---

# Read-only behavior

The buffer should be externally read-only from creation onward, but internal process appends must bypass that restriction.

The current special-buffer path sets `readonly_override` and clears dirty state, so preserve that behavior in the new prepare API.

Internal appends should:

* Not produce undo entries.
* Reset `dirty` to zero.
* Not change `readonly_override`.
* Not trigger “Buffer is read-only”.
* Not make the buffer eligible for save prompts.

---

# Window behavior and tail following

## Initial display

Use the existing display-buffer policy:

* If a window already displays `*compilation*`, leave it there.
* With one sufficiently large window, split horizontally.
* With multiple windows, reuse another window.
* Keep focus in the source window.

Add a helper such as:

```c
void win_display_buffer_other_window_at_end(int buffer_index);
```

or:

```c
void win_move_buffer_windows_to_end(int buffer_index);
```

The initial header should be visible immediately, preferably with the window positioned at its end.

## Follow-tail policy

Before appending, record for every window displaying the compilation buffer whether its point/view was at the previous end.

After appending:

* Windows that were at the end move to the new end.
* Windows that were not at the end retain their cursor and `rowoff`.
* Clamp invalid positions if truncation or clearing changed the row count.

This gives natural behavior without adding a persistent “follow mode” flag:

* Initially, the compilation window follows.
* Once the user scrolls upward, it stops following.
* Moving back to the bottom re-enables following automatically.

Do not force the active compilation window to the bottom merely because new output arrived.

---

# Starting and restarting compilation

Refactor the duplicated bodies of `editor_compile()` and `editor_recompile()` around a shared entry point:

```c
static void compilation_start(
    const char *command,
    const char *directory,
    int source_buffer);
```

## `editor_compile()`

Continue to:

* Seed the minibuffer from `editor.compile_command`.
* Allow editing.
* Store the edited command as a user override.
* Resolve the source directory.
* Record source buffer.
* Call `compilation_start()`.

## `editor_recompile()`

Continue to preserve current semantics:

* From `*compilation*`, use the last command and last directory.
* From a source buffer, use its compile command and directory.
* Clear and reuse `*compilation*`.

The current implementation already distinguishes those cases.

## Active-process policy

Before clearing the buffer, check whether a process is running.

Recommended behavior:

```text
A compilation process is running; kill it? (y/n)
```

On yes:

1. Signal the old process group.
2. Continue polling until it is reaped.
3. Start the new process.

Do not immediately overwrite the state struct while the previous PID is still unreaped.

An alternative is to mark a pending restart and launch it after finalization. That avoids any blocking wait:

```c
bool restart_pending;
char pending_command[...];
char pending_directory[...];
```

This is the strongest design if restart must remain fully asynchronous.

---

# Cancellation and process cleanup

Add:

```c
void editor_kill_compilation(int fd);
void compilation_shutdown(void);
```

## `kill-compilation`

Recommended sequence:

1. Verify a compilation is active.
2. Send `SIGINT` to `-process_group`.
3. Mark phase `COMPILATION_TERMINATING`.
4. Update status.
5. Continue normal nonblocking polling.

Optionally escalate:

* After a grace interval, a second `kill-compilation` sends `SIGKILL`.
* Or send `SIGTERM` after one second and `SIGKILL` after another delay.

The minimal version can use SIGTERM immediately, but SIGINT is more compilation-friendly.

## Editor exit

`editor_cleanup()` or another shutdown hook must:

* Close the output descriptor.
* Signal the process group.
* Reap the child.
* Avoid leaving compilation descendants running after kg exits.

Because exit cleanup is already terminal-oriented and synchronous, a bounded termination sequence is acceptable there.

## Killing `*compilation*`

Choose one explicit behavior:

* **Recommended:** killing the buffer while its process is active first asks to kill the process.
* Simpler alternative: reject buffer deletion with “Compilation is still running”.

Do not let the process retain a stale buffer index after the buffer slot has been freed and reused.

---

# Output cap and memory behavior

Preserve the current 8 MiB maximum.

Once the cap is reached:

1. Append exactly one marker:

```text
[kg: compilation output truncated after 8388608 bytes]
```

2. Continue reading and discarding data.
3. Do not close the read end early; doing so could deliver SIGPIPE to the build and change its behavior.
4. Still append the final exit-status line.

Count the cap against child output, not the kg-generated header and footer.

For very large output, avoid one allocation proportional to the entire transcript. The buffer’s row representation already naturally stores output incrementally.

---

# ANSI and colored output

## Why ANSI needs separate work

Currently ESC and other control bytes are classified as non-printing and rendered as visible reverse-video control symbols.

The renderer obtains colors from each row’s `hl` array and translates those semantic values into terminal color escapes.

Therefore, simply retaining ANSI sequences in `row->chars` would show `^[`-style garbage rather than applying colors.

## Phase 1: safe ANSI stripping

Even without rendered colors, add an incremental escape-sequence sanitizer so colored tools do not pollute the buffer.

It should handle sequences split across arbitrary pipe reads:

* CSI: `ESC [ ... final-byte`
* OSC: `ESC ] ... BEL` or `ESC ] ... ESC \`
* Simple two-byte escape sequences
* Incomplete sequence retained until the next chunk

Policy:

* Strip SGR sequences.
* Strip unsupported cursor/control sequences.
* Never pass raw ESC bytes into the compilation buffer.
* Preserve ordinary UTF-8.

Also define behavior for:

* `\r\n`: newline.
* Bare `\r`: replace/reset the pending logical line, useful for progress indicators.
* Backspace: remove the previous pending-line byte where safe.
* NUL and other controls: discard or render through an explicit escaped form.

This clean-output parser is worthwhile even when color rendering is deferred.

## Phase 2: SGR rendering

For actual colors, add an optional per-byte display style separate from syntax highlighting.

A possible addition to `erow`:

```c
uint16_t *style;
uint16_t *render_style;
```

Packed style bits could represent:

* No override.
* ANSI foreground color 0–15.
* 256-color foreground 0–255.
* Bold.
* Possibly underline.

The parser maps:

* `0`: reset.
* `1`: bold.
* `22`: normal intensity.
* `30–37`, `90–97`: foreground colors.
* `39`: default foreground.
* `38;5;n`: 256-color foreground.

Defer truecolor `38;2;r;g;b` unless it is specifically desired; supporting it cleanly needs RGB storage rather than a compact palette index.

## Renderer precedence

For compilation rows:

1. Region/selection reverse video remains highest priority.
2. ANSI style overrides syntax foreground.
3. Syntax highlighting applies when there is no ANSI override.
4. Reset attributes at every row and window boundary to prevent color leakage.

Tab expansion must copy the tab byte’s style to all generated spaces in `render_style`.

## Do not use a PTY just to obtain colors

With a normal pipe, many build tools decide they are not connected to a terminal and suppress color. Users can still configure commands such as `--color=always`.

A PTY would make color automatic for more tools, but then kg must handle:

* Terminal width.
* Carriage-return rewriting.
* Cursor movement.
* Programs attempting to read stdin.
* Job-control signals.
* PTY EOF semantics.
* Potentially interactive subprocesses.

That is closer to a terminal emulator than a compilation buffer and is not justified for the first implementation.

---

# Commands and keybindings

Add an M-x command table entry:

```c
{ "kill-compilation", editor_kill_compilation, CMD_NONE },
```

The command table is already the canonical M-x registration point for `compile` and `recompile`.

For the optional compilation-mode binding:

* In `*compilation*`, `C-c C-k` invokes `kill-compilation`.
* Outside that buffer, retain existing C-c behavior.
* Alternatively defer the keybinding and expose only M-x initially.

A buffer-specific key is user-visible behavior, so update built-in help and documentation as required by the repository guidance.

---

# File-by-file implementation plan

## `src/compile.h`

* Expand `compilation_state`.
* Add lifecycle enum.
* Add:

  * `compilation_poll()`
  * `compilation_is_running()`
  * `editor_kill_compilation()`
  * `compilation_shutdown()`
* Remove or retain `compilation_format_transcript()` only for compatibility/tests.
* Add parser declarations if ANSI parsing remains local to compilation.

## `src/compile.c`

* Deduplicate compile/recompile start logic.
* Implement asynchronous spawn.
* Implement nonblocking drain.
* Implement line assembly.
* Implement output-cap handling.
* Implement `waitpid(WNOHANG)` finalization.
* Implement cancellation and shutdown.
* Create/display the buffer before returning.
* Preserve last command/directory and source-buffer behavior.

## `src/bufmgr.c`

* Split special-buffer creation/reset from replacement.
* Add append-to-special-buffer API.
* Implement background mutation without switching the active window.
* Keep internal appends clean and undo-free.
* Preserve read-only state.
* Optionally make `buf_replace_special_text()` a wrapper over the new API.

## `src/def.h`

* Export the new buffer APIs.
* Export compilation polling/shutdown where appropriate.
* Add row style fields only in the ANSI-rendering phase.

## `src/tty.c`

* Call `compilation_poll()` from timed input loops.
* Redraw from `editor_read_key_idle()` when output changed.
* Drain without forced redraw from minibuffer-oriented reads.

## `src/main.c`

* Poll compilation near other periodic editor work.
* Ensure shutdown executes before final return if not already covered by cleanup.

## `src/winmgr.c`

* Add an at-end display helper or general window-to-buffer-end helper.
* Preserve “reuse existing display” behavior.
* Add tail-follow calculations based on whether a window was previously at the end.

## `src/cmd.c`

* Register `kill-compilation`.

## `src/kbd.c`

* Optionally route `C-c C-k` in the compilation buffer to `kill-compilation`.
* Preserve git-commit-mode’s existing use of the same key.

## `src/display.c`, `src/buffer.c`, `src/syntax.c`

Only required for actual ANSI rendering:

* Allocate/free/copy style arrays.
* Expand styles alongside tabs.
* Render ANSI color attributes.
* Keep semantic syntax highlighting intact elsewhere.

## Documentation

Update:

* `README.md`: replace “synchronous” with streaming/asynchronous behavior. It currently explicitly documents compilation as synchronous.
* `doc/kg.1`
* `src/help.c` when adding `C-c C-k`
* Possibly `doc/TODO.md`

---

# Testing plan

The repository expects both native unit tests and PTY acceptance tests, with full validation through `make check` and the numbered CI scripts.

## Native tests

Extend `test/test_compile.c`.

The current tests cover only transcript formatting and directory resolution.

Add tests for:

### Lifecycle

* Initial state is idle.
* Successful start records PID, FD, command and directory.
* Start returns before child completion.
* EOF without reaped child does not finalize prematurely.
* Reaped child with unread data does not finalize prematurely.
* Finalization occurs once both conditions are met.
* Exit code is preserved.
* Signal termination is preserved.
* State returns to idle.
* Descriptor is closed exactly once.

### Incremental output

Feed chunks such as:

```text
"hel"
"lo\nwor"
"ld\n"
```

Assert rows become:

```text
hello
world
```

Test:

* Empty chunks.
* Multiple lines in one chunk.
* Final line without newline.
* A newline split across polling calls.
* UTF-8 split across reads.
* Very long lines.
* Header before child output.
* Footer after pending final output.

### Output cap

* Data below cap is retained.
* Crossing cap keeps only allowed bytes.
* Truncation marker appears once.
* Additional data is drained but not stored.
* Completion footer remains present.

### Poll fairness

Use a fake reader or injected read callback to return unlimited data and assert one call consumes no more than the configured budget.

### Cancellation

* SIGINT targets the negative process-group ID.
* Repeated kill behavior follows the selected escalation policy.
* Cancellation still drains remaining output.
* Shutdown reaps the process.

### ANSI sanitizer

Even when color rendering is deferred:

* SGR split across chunks.
* OSC split across chunks.
* Unsupported CSI.
* Reset.
* Bare ESC at EOF.
* CRLF.
* Bare CR progress updates.
* Backspace.
* Ordinary UTF-8 unaffected.

### ANSI rendering phase

* Basic and bright colors.
* Reset and default foreground.
* Bold on/off.
* 256-color sequence.
* Style spanning chunk boundaries.
* Style spanning multiple lines.
* Tabs inherit style across expanded spaces.
* No terminal attributes leak after the compilation window.

## PTY acceptance tests

### 1. Editor remains interactive

Use a source file containing a compile command that sleeps for considerably longer than the test timeout:

```text
-*- compile-command: "printf started; sleep 10; printf finished" -*-
```

Sequence:

1. Start compilation.
2. Immediately edit the source buffer.
3. Save.
4. Quit kg before the child would naturally finish.

Expected:

* The edit is saved.
* kg exits within the short test timeout.
* Cleanup terminates the child.

The old synchronous implementation would remain blocked in `sleep 10`, making this a strong regression test.

### 2. Compilation buffer appears immediately

Start a command that prints one line and then sleeps.

Assert the tmux screen contains:

* `*compilation*`
* The start header.
* The first line.

Do this before allowing the process to finish.

### 3. Streaming order

Command:

```sh
printf 'one\n'; sleep 1; printf 'two\n' >&2
```

Assert the final buffer contains `one` before `two`.

### 4. Read-only buffer

While compilation is running or after it completes:

* Select `*compilation*`.
* Attempt an editing command.
* Assert “Buffer is read-only”.
* Assert output contents remain unchanged.

### 5. Recompile

* Complete one command.
* Change or reuse the command.
* Run `recompile`.
* Assert the old transcript is replaced, not appended.
* Assert last directory behavior remains correct.

### 6. Kill compilation

* Start `sleep 10`.
* Invoke `kill-compilation`.
* Assert responsive return.
* Assert a signal-termination footer.
* Quit without timeout.

### 7. Window policy

Cases:

* One window: horizontal split appears.
* Existing compilation window: reused.
* Multiple windows: no unnecessary extra split.
* Focus remains in source window.
* User scrolls compilation window up; later output does not snap it down.

PTY tests are the correct place for window and interactive behavior; the repository explicitly recommends them for behavior dependent on real terminal input, cursor movement and windows.

---

# Suggested implementation sequence

## Commit 1: Process lifecycle

* Async spawn.
* Nonblocking polling.
* Reaping.
* Cancellation and cleanup.
* No buffer streaming yet; use an internal test sink.

## Commit 2: Incremental special-buffer append

* Prepare/clear/append APIs.
* Immediate split.
* Streaming rows.
* Read-only behavior.
* Output cap.
* Tail following.

At this point the primary feature is complete.

## Commit 3: Tests and documentation

* Unit lifecycle/parser tests.
* PTY responsiveness and cancellation tests.
* README/man/help updates.

## Commit 4: ANSI sanitization

* Strip control sequences.
* Handle fragmented escapes, CR and backspace.

## Commit 5: ANSI color rendering

* Extend row attributes.
* Parse SGR.
* Render 8/16 and optionally 256 colors.
* Add focused renderer/parser tests.

Separating ANSI from asynchronous execution keeps the high-risk display change from obscuring process-lifecycle bugs.

---

# Definition of done

The feature is complete when all of these hold:

* `M-x compile` returns control immediately after successful spawn.
* `*compilation*` is created and displayed before the child exits.
* stdout and stderr stream incrementally.
* Editing remains responsive during a long-running build.
* Output is drained during minibuffer prompts.
* Resize and other periodic editor work continue.
* `*compilation*` is read-only to the user.
* Recompile clears and reuses the buffer.
* Tail following stops when the user scrolls away.
* The 8 MiB cap remains effective without breaking the child.
* Exit code and signal status are reported correctly.
* Cancellation targets the whole process group.
* Exiting kg leaves no child and no zombie.
* Raw ANSI escapes never appear visibly.
* Colored output, when included, cannot leak terminal attributes outside its window.
* `make check`, both Lisp configurations, sanitizers, format checks and the repository CI scripts remain green.

