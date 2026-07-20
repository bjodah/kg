## Findings

### [P1] The final unterminated output line is duplicated

`compilation_process_bytes()` appends the complete pending line to `*compilation*` after every read, while retaining the same bytes in `pending_line`.

At EOF, `compilation_poll()` calls `compilation_flush_pending()` without first removing that displayed copy. The flush appends the pending line again, followed by a newline.

For example:

```sh
printf foo
```

will produce approximately:

```text
foofoo

Compilation finished with exit code 0
```

The existing synchronous code had an explicit no-final-newline test, but that test does not exercise the new streaming path.

**Fix:** track how many pending bytes are currently displayed and remove them before the final flush, or redesign the append path so pending bytes are displayed by modifying the last row without treating them as committed output.

---

### [P1] The restart confirmation has a process-completion race

Both `compile` and `recompile` check `compilation_is_running()`, display a confirmation, and wait for input. After the user answers yes, they unconditionally signal the recorded process group and change the phase to `COMPILATION_TERMINATING`.

However, `editor_read_key()` calls `compilation_poll()` on every 100 ms input timeout. The compilation can therefore finish and be fully finalized while the confirmation is still visible.

After that happens, the confirmation path can:

1. Send SIGINT to a stale process-group ID.
2. Change an already-idle state back to `COMPILATION_TERMINATING`.
3. Cause the completed compilation to be finalized a second time because `pipe_eof` and `child_reaped` remain true.
4. In the worst case, signal an unrelated process group if the PID has already been reused.

**Fix:** immediately re-check `compilation_is_running()` after `editor_read_key()` returns. When it has already finished, start the requested command directly. Also clear PID/process-group and lifecycle flags during finalization so stale state cannot be resurrected.

---

### [P1] The 8 MiB output accounting repeatedly counts the current partial line

Before processing a new chunk, the implementation removes the previously displayed pending line from the buffer.

After processing the chunk, it appends the entire pending line again and increments `stored_output` by the entire line length. It never subtracts the bytes removed at the beginning.

Consequently, a line split across reads is counted repeatedly. A long line arriving in 4 KiB chunks is charged approximately:

```text
4 KiB + 8 KiB + 12 KiB + ...
```

rather than once per actual byte. An unbroken output line can therefore trigger the nominal 8 MiB limit after only a few hundred KiB of real output.

This accounting error also interacts badly with `buf_truncate_last_row()`: after only part of a pending line fits under the cap, the next truncation requests removal of the full pending-line length. The helper does nothing when that length exceeds the displayed row size.

**Fix:** distinguish:

* bytes permanently committed to the transcript;
* bytes held in `pending_line`;
* bytes from `pending_line` currently mirrored in the last row.

Only newly received child-output bytes should consume the output allowance. A `displayed_pending_length` field would make replacement and truncation unambiguous.

---

### [P2] A queued restart does not update `last_command` or `last_directory`

When no compilation is active, `editor_compile()` records the accepted command and directory before starting it.

When replacing an active compilation, it stores the new command only in `pending_command` and `pending_directory`; it does not update the `last_*` fields.

`compilation_start()` also does not update those fields. Later, `recompile` invoked from `*compilation*` obtains its command and directory from the stale `last_*` fields.

Reproduction:

1. Start command A.
2. While A is running, invoke `compile`, enter command B and approve termination.
3. Let B finish.
4. Invoke `recompile` from `*compilation*`.
5. Command A runs instead of B.

**Fix:** make `compilation_start()` the single place that records the command and directory that actually started. That avoids differences between ordinary starts and deferred starts.

---

### [P2] A deferred restart can change buffers while an unrelated minibuffer is active

When an interrupted child finally exits, `compilation_poll()` starts the queued replacement immediately.

`compilation_start()` saves the current state and restores `pending_source_buffer`, potentially changing the active buffer and window underneath the current operation.

Because ordinary minibuffer reads call `compilation_poll()`, this can occur while the user is in `find-file`, `M-x`, a save confirmation, or another prompt.

The prompt code will continue operating, but its underlying active buffer may unexpectedly have changed.

**Fix:** let polling mark the restart as ready, but launch it only from the top-level editor loop. Also avoid restoring the original source buffer during a delayed restart; retain whatever buffer/window the user currently occupies and display compilation output in the other window.

---

## Test coverage

The commit adds the interfaces required to link `compile.c`, but the additions to `test_compile.c` are only stubs for screen, input, buffer and window functions.

The executed tests remain the old synchronous transcript-formatting and directory-resolution tests. None exercises:

* incremental chunks;
* final output without `\n`;
* partial lines split across reads;
* output-cap accounting;
* ANSI sequences split across chunks;
* child exit versus pipe-EOF ordering;
* queued restart;
* compilation completion during a confirmation prompt;
* cancellation and process-group cleanup.

The first three findings would likely have been caught by small deterministic tests around the byte-processing state machine, without spawning real processes.

## Overall assessment

The architecture is broadly aligned with the plan: nonblocking pipe reads, bounded per-tick work, `waitpid(..., WNOHANG)`, process-group cancellation, background polling and slot-oriented special-buffer mutation are all sensible choices. The partial-line representation is currently doing too many jobs at once, though, leading to both the duplication and accounting failures. I would address the three P1 findings before relying on this for normal compilation work.

