# kg Editor Implementation & Improvement Plan

This document outlines a structured plan to improve the kg editor codebase based on a comprehensive code review. It is organized by priority and serves as a guide for junior developers to start contributing to the project.

## 1. Critical Fixes (Data Safety & Crashes)
These issues should be addressed immediately as they risk data corruption or application crashes.

### 1.1 Signal Handler Safety
- **File:** `src/tty.c` (lines 658-668)
- **Issue:** `handle_sig_winch` calls `update_window_size()` and `editor_refresh_screen()`, which internally use `malloc`/`realloc`/`snprintf`. These functions are not async-signal-safe.
- **Task:** Modify the `SIGWINCH` handler to just set a `volatile sig_atomic_t winch_pending = 1;` flag. Check this flag in the main event loop (e.g., inside `editor_process_keypress` or `editor_refresh_screen` calls) and perform the actual reflow and refresh there.

### 1.2 Integer Overflow in Buffer Accumulation
- **File:** `src/buffer.c` (lines 390-393 in `editor_rows_to_string`)
- **Issue:** `totlen` is an `int`. For large files, `totlen += rows[j].size` can overflow `INT_MAX`, causing a tiny buffer allocation and a subsequent heap buffer overflow.
- **Task:** Change `totlen` to `size_t` or add an overflow check: `if (totlen > INT_MAX - rows[j].size) return NULL;`

## 2. High Priority Improvements
These issues affect robustness and performance in edge cases.

### 2.1 File Save Atomicity (Truncate + Write Race)
- **File:** `src/fileio.c`
- **Issue:** `ftruncate` followed by `write` leaves the file truncated if the editor is killed between the operations.
- **Task:** Implement safe saving: write to a temporary file (e.g., `filename.tmp` using `mkstemp`), flush/sync it, and then use `rename()` to atomically overwrite the original file.

### 2.2 Error Handling on File Open
- **File:** `src/fileio.c` (lines 118-119)
- **Issue:** If `fopen` fails for reasons other than `ENOENT` (like Permission Denied), the editor calls `perror` and `exit(1)`. This immediately kills the editor and drops unsaved data in other buffers.
- **Task:** Replace the `exit(1)` with a graceful return: `editor_set_status_message("Cannot open %s: %s", filename, strerror(errno)); return 1;`

### 2.3 `UNDO_YANK_TEXT` Performance
- **File:** `src/undo.c` (lines 221-233)
- **Issue:** Undoing a yank (paste) loops calling `editor_del_forward_char()` character-by-character. For large pastes, this results in O(n²) performance due to continuous memory shifting.
- **Task:** Implement a bulk delete operation (similar to `editor_delete_region_range`) for yanks to process the undo in O(n) time.

### 2.4 Unsafe `erow` Shallow Copies during Sort
- **File:** `src/yank.c` (lines 474-487)
- **Issue:** `editor_sort_lines` shallow-copies `erow` structs via `memcpy` and sorts them with `qsort`. This leads to aliased pointers for `chars`, `render`, and `hl` fields, which is extremely fragile if memory is freed during the process.
- **Task:** Modify the sort implementation to either sort an array of row indices and reorder the original array in-place via swaps, or sort pointers rather than struct copies.

## 3. Medium / Low Priority Code Quality Tasks
These tasks will clean up technical debt and improve code maintainability.

### 3.1 Unhandled OOM in `ab_append`
- **File:** `src/display.c` (lines 32-42)
- **Issue:** If `realloc` fails, `ab_append` silently drops data, resulting in garbled screen output.
- **Task:** Implement an OOM flag (e.g., `ab_oom`) to catch allocation failures and return early from screen refresh, possibly displaying an "Out of memory" status message.

### 3.2 Refactor `editor_process_keypress`
- **File:** `src/kbd.c`
- **Issue:** The `editor_process_keypress` function is a monolithic ~700-line switch statement, making it hard to read and unit test.
- **Task:** Break this switch statement down by extracting related key bindings into modular handler functions (e.g., `handle_navigation_keys`, `handle_edit_keys`) or implement a dispatch table.

### 3.3 Strict Constants and Overflow Checks
- **Files:** `src/syntax.c`, `src/yank.c`, `src/word.c`
- **Task:**
  - Add `const` to syntax keyword arrays (e.g., `const char *const C_HL_extensions[]`).
  - Fix integer overflow vulnerability in `kill_ring_append` (`killring.len + len + 1`). Add an explicit `INT_MAX` check.
  - Dynamically size or ensure `removed` array in `editor_comment_dwim` (`src/word.c`) accommodates the maximum size of `singleline_comment_start`.

### 3.4 Missing Memory Cleanup on Exit
- **File:** `src/main.c`
- **Issue:** The editor doesn't free allocated memory for undo stacks, rows, or the kill ring upon exit.
- **Task:** Create a `cleanup_all()` function registered via `atexit` (or called at the end of `main`) to free all buffers, undo stacks, and global state. This makes running memory sanitizers like Valgrind much cleaner.

## 4. Testing Gaps to Address
- **Unit Tests for Commands:** `src/cmd.c` contains named commands but lacks unit tests for the dispatch mechanism.
- **Display and Layout:** Add mock-based tests for `src/display.c` and `src/winmgr.c` to test window splits, line reflowing, and visual-line mode edge cases without a real PTY.
- **Macro Recording:** Implement tests for `src/macro.c` to verify start, stop, replay, and buffer limit bounds.
