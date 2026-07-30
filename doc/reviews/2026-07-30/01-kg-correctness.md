# kg core correctness and safety review

Date: 2026-07-30
Scope: primarily `src/`, `test/`, and `utils/`; product code was not changed.

## Executive summary

The strongest findings are two ordinary editing failures, not exotic static-analysis edge cases:

1. Backspace and forward-delete corrupt any multi-byte UTF-8 character.
2. Undo after `C-k` at end-of-line restores the wrong bytes and corrupts the text.

Both are high-confidence, user-reachable data-integrity bugs and should be fixed before larger architectural work. The asynchronous compilation output limit also has two independent bypasses that can make kg consume memory without bound. Disk-change detection then has a smaller but important data-loss window: it treats a missing file as unchanged and compares only whole-second mtime plus size.

The existing test suite is broad and healthy, but its UTF-8 coverage is concentrated on insertion, movement, display width, search, and minibuffers; the ordinary buffer deletion paths have no multi-byte regression cases. Likewise, the one `kill-line` undo unit case exercises content-to-EOL but not the distinct newline-at-EOL branch.

## Findings

### KG-C01 — Backspace and forward-delete remove one byte, not one UTF-8 character

- Severity: **High**
- Confidence: **Confirmed / very high**
- Evidence:
  - `src/basic.c:149-183` deliberately moves left/right by a whole UTF-8 glyph and therefore leaves `filecol` on glyph boundaries.
  - `src/buffer.c:1195-1203` records and removes exactly `row->chars[filecol - 1]` for Backspace.
  - `src/buffer.c:1236-1239` records and removes exactly `row->chars[filecol]` for forward-delete.
  - `src/buffer.c:1321-1329` explicitly documents that byte-at-a-time undo/insertion would leave invalid UTF-8, but deletion does precisely that.
- Failure scenario:
  - In a row containing `aéz`, place point after `é` and press Backspace. Point is after the two-byte sequence, but only its final continuation byte is removed. The buffer becomes invalid UTF-8.
  - Place point before `漢` and press `C-d`; only the three-byte glyph's lead byte is removed, leaving two stray continuation bytes.
  - A subsequent undo restores only the one byte recorded by `UNDO_DELETE_CHAR`; it does not make these operations character-granular.
- Test direction:
  - Add native buffer/undo cases for Backspace and forward-delete over 2-, 3-, and 4-byte characters, asserting exact bytes, point, and single-step undo.
  - Add one PTY saved-file case for each command so dispatch/read-only behavior is included.
- Fix direction:
  - Compute the preceding glyph start with `utf8_glyph_start_before()` for Backspace and the span with `utf8_glyph_span_at()` for forward-delete.
  - Record the entire deleted byte span, preferably through `UNDO_REPLACE_TEXT`/a generalized span deletion record rather than extending the single-byte `c` field.
  - Move point by the deleted byte span while preserving viewport behavior.

### KG-C02 — Undo after killing an end-of-line newline duplicates the next line

- Severity: **High**
- Confidence: **Confirmed / very high**
- Evidence:
  - At EOL, `src/buffer.c:1379-1391` removes only the logical newline by joining the next row, and appends `"\n"` to the kill ring.
  - The undo record at `src/buffer.c:1384-1387` incorrectly stores the entire next row instead of the removed newline.
  - `src/undo.c:219-224` reverses `UNDO_KILL_TEXT` by inserting the recorded bytes at the original point.
  - The existing unit case at `test/test_undo.c:195-210` covers only killing `"llo"` inside one line, not the EOL branch.
- Failure scenario:
  - Start with `a\nb`, move to the end of `a`, press `C-k`, then undo. `C-k` produces `ab`; undo inserts the recorded `"b"` and produces `abb`, rather than restoring `a\nb`.
  - This is a normal editing sequence and the corrupted result can be saved without further warning.
- Test direction:
  - Extend `test/test_undo.c` with `a\nb -> C-k at EOL -> ab -> undo -> a\nb`.
  - Add a PTY saved-file regression for the same sequence.
  - Also cover empty and multi-byte next rows, plus batched `C-u N C-k`.
- Fix direction:
  - Record `"\n"` with length 1 for the EOL half-step, matching the bytes put in the kill ring and the bytes logically removed.
  - Consider making `editor_kill_line()` produce one explicit `(removed bytes, length)` result so kill-ring and undo payloads cannot diverge.

### KG-C03 — The 8 MiB compilation output cap is bypassable and pending lines are unbounded

- Severity: **High**
- Confidence: **Confirmed / high**
- Evidence:
  - `src/compile.c:409-423` doubles `pending_line` as characters arrive with no relationship to `maximum_output`.
  - `src/compile.c:431-445` charges only `pending_line_length` to `stored_output`; the newline appended at line 444 is not charged.
  - `src/compile.c:494-514` commits on every newline but otherwise continues accumulating the pending line.
  - `test/test_compile.c:303-325` covers final unterminated-line duplication, but there is no cap-boundary, newline-only, or very-long-line streaming case.
- Failure scenario:
  - A compiler/tool emitting one extremely long line causes `pending_line` to grow far beyond 8 MiB before it is truncated at commit time. A command producing hundreds of MiB without a newline can exhaust memory.
  - A command producing only newlines never advances `stored_output`, so every newline is appended to the compilation buffer indefinitely despite the advertised cap.
- Test direction:
  - Refactor/expose a small state-machine seam so tests can set a tiny cap (for example 8 bytes), feed chunks directly, and assert total retained bytes and the truncation marker.
  - Cover a single over-cap line, newline-only output, a line exactly at the cap, and chunk boundaries around CR/LF/ANSI sequences.
- Fix direction:
  - Charge every retained output byte, including newlines, against one budget.
  - Stop growing `pending_line` once the remaining budget is exhausted; continue draining/parsing only enough state to finish the process safely.
  - Add checked capacity doubling and propagate allocation failure rather than silently dropping characters.

### KG-C04 — External-change detection misses deletion and same-size rewrites within one second

- Severity: **High** for overwrite risk, **Medium** for auto-revert behavior
- Confidence: **Confirmed / high**
- Evidence:
  - `src/fileio.c:20-31` snapshots only `st_mtime` (whole seconds in the stored `time_t`) and size.
  - `src/fileio.c:37-44` returns “unchanged” for every `stat()` failure and otherwise compares only those two values.
  - `src/fileio.c:476-484` relies on that predicate before saving without an overwrite confirmation.
  - `src/bufmgr.c:266-310` relies on the same predicate for auto-revert.
- Failure scenario:
  - Open a file, let another process delete or rename it away, edit the buffer, and save. `stat()` returns `ENOENT`, `file_state_differs()` reports false, and kg recreates the pathname without warning.
  - Rewrite the open file externally with different bytes of the same length while preserving or colliding within the same one-second mtime. kg reports no change; a later save overwrites the external edit without confirmation.
  - Permission/transient I/O errors are also classified as “unchanged,” hiding the fact that kg could not check.
- Test direction:
  - Add file-I/O tests for deletion after snapshot, same-size/same-second replacement, and a forced `stat` error.
  - Verify both save prompting and auto-revert flag behavior.
- Fix direction:
  - Store nanosecond-resolution `struct timespec` (`st_mtim`/portable equivalent), size, and preferably device/inode identity.
  - Represent the comparison as `same`, `different`, or `unknown/error`; an existing snapshot followed by `ENOENT` is “different,” while other errors should block or warn rather than mean “same.”

### KG-C05 — `write-file` can overwrite an existing destination without the intended existence prompt

- Severity: **Medium**
- Confidence: **Confirmed code path / high; collision-dependent failure**
- Evidence:
  - The ordinary save-from-special-buffer path explicitly checks destination existence at `src/fileio.c:454-462`.
  - `editor_write_file()` at `src/fileio.c:517-540` adopts the new name first and delegates to `editor_save()` without an unconditional existence check.
  - Since the buffer is then non-special, `editor_save()` uses only `file_state_differs(new_path, old_buffer_mtime, old_buffer_size)` at `src/fileio.c:476-484`.
- Failure scenario:
  - Buffer A and existing destination B have the same byte length and whole-second mtime (common after a batch checkout/copy). `C-x C-w B` makes the metadata comparison return false and overwrites B without the “File B exists. Overwrite?” confirmation promised by the other write-name path.
- Test direction:
  - Create source and destination with different contents, equal sizes, and equal mtimes; invoke or unit-drive `editor_write_file()` and assert that confirmation is requested before any rename.
- Fix direction:
  - Perform an unconditional destination existence check in `editor_write_file()` before mutating `editor.filename`, just as the special-buffer branch does.
  - Keep external-change detection separate from “new destination already exists” policy.

### KG-C06 — Undo reconstruction can violate the NUL-terminated row invariant

- Severity: **Medium**
- Confidence: **Confirmed invariant violation / high; downstream impact is operation-dependent**
- Evidence:
  - `editor_insert_row()` allocates `len + 1` and copies `len + 1` bytes from the caller at `src/buffer.c:325-340`, assuming the byte after the requested slice is already NUL.
  - Rectangle undo passes newline-delimited interior slices at `src/undo.c:265-280`.
  - Paragraph-reflow undo passes newline-delimited interior slices at `src/undo.c:301-314`; the source snapshot is explicitly built with internal newlines at `src/word.c:1128-1143`.
  - NUL-dependent consumers exist: `sort_lines_cmp()` calls `strcmp(row->chars, ...)` at `src/yank.c:583-589`.
- Failure scenario:
  - Undoing reflow or a multi-row rectangle reconstructs each non-final row with `chars[size] == '\n'`, not `'\0'`. Length-aware rendering/saving often masks the error, but a later `sort-lines` can compare through the logical row end into subsequent snapshot bytes, yielding wrong ordering. Any future/string-based consumer inherits the same latent over-read risk.
- Test direction:
  - After multi-row reflow undo and rectangle undo, assert `row[i].chars[row[i].size] == '\0'` for every row.
  - Follow with `sort-lines` in a regression case that would differ if comparison crosses the row boundary.
- Fix direction:
  - Change `editor_insert_row()` to copy exactly `len` bytes and set `newchars[len] = '\0'` itself.
  - Audit similar APIs so `(pointer, length)` never implicitly requires an accessible terminator.

### KG-C07 — A malformed UTF-8 sequence consumes and drops the following valid key

- Severity: **Low**
- Confidence: **Confirmed state-machine behavior / high**
- Evidence:
  - `src/tty.c:493-512` reads continuation bytes eagerly through `editor_read_raw_byte()`.
  - On a non-continuation byte, lines 506-508 return failure without putting that already-consumed byte back into `pending_input`.
- Failure scenario:
  - Input bytes `E2 41` (truncated/malformed UTF-8 lead followed by `A`) cause kg to discard both the malformed lead and the valid `A`. This can arise from a broken terminal/transport or arbitrary byte-oriented input injection.
- Test direction:
  - Drive the input reader through a pipe with a malformed lead followed by ASCII and assert that the ASCII byte becomes the next key.
- Fix direction:
  - Add a one-byte pushback path to the pending-input queue when a promised continuation is not one, while still dropping the malformed lead.
  - Validate scalar restrictions as well as continuation structure if the input contract is “well-formed UTF-8 only.”

## Suspicions worth a later focused pass

These were not promoted to findings because I did not validate a concrete user-visible failure in this bounded review:

- `src/winmgr.c:105-140` can calculate zero/negative window text heights and widths on very small terminals or after enough splits. Much display/movement code now guards non-positive dimensions, but a dedicated `dimensions: [1, 1]`/resize stress suite would be worthwhile.
- `src/fileio.c:563-627` stores `read()` into `size_t`, converts it back to `ssize_t` to detect `-1`, and performs unchecked growth arithmetic. It works on common two's-complement ABIs for normal file sizes but should use `ssize_t n` and checked `size_t` arithmetic.
- Synchronous `shell_run()` capture grows an `int` capacity without an explicit output cap; a shell command with enormous output can block the editor and eventually overflow capacity arithmetic. The asynchronous compilation path is the more immediate priority because it already claims to impose a cap.

## Positive observations

- File loading is transactional (`load_file_transactional()`/`commit_load_result()`), avoiding partial replacement of the live buffer on read/allocation failure.
- Atomic save handles short writes and `EINTR`, fsyncs before rename, cleans temporary files on failure, and has focused injection tests.
- Multi-byte self-insert and overwrite paths consciously use span-based undo; the comments around `editor_self_insert_glyph()` correctly state the invariant that the deletion paths currently miss.
- Cursor movement, display columns, tabs, wide characters, combining marks, and codepoint/byte conversion have unusually strong focused tests for a small editor.
- Async compilation separates pipe EOF from child reaping, drains without blocking, strips common ANSI/CR progress output, and avoids starting deferred restarts under minibuffer prompts.
- Read-only enforcement, minibuffer overflow rejection for executable commands, shell-output escape rejection, load bounds, and numerous stale-cursor cases show good defensive intent.

## Top five priorities

1. **Fix character-granular Backspace/Delete and add native + PTY UTF-8 deletion tests (KG-C01).**
2. **Fix EOL `kill-line` undo payload and cover both single and batched kills (KG-C02).**
3. **Make compilation retention a true byte budget, including pending lines and newlines (KG-C03).**
4. **Strengthen disk identity/change tracking and distinguish missing/error from unchanged (KG-C04).**
5. **Make every row constructor own NUL termination, then assert the invariant broadly (KG-C06).**

After those, close the `write-file` overwrite-confirmation gap (KG-C05), then harden malformed terminal input and tiny-terminal geometry.

## Diagnostics performed

- Read `README.md` first, then reviewed the relevant buffer, undo, file-I/O, terminal/input, window, shell, compilation, buffer-manager, and test paths.
- `make -j2`: passed.
- `make check-unit`: 17/17 native suites passed.

The green baseline is useful: the confirmed failures above identify missing behavioral cases rather than known failures in the current suite.
