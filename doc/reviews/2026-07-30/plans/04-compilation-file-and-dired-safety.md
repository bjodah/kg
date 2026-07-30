# Plan 04 — Compilation output, file identity, and Dired safety

## Goal

Make byte/resource limits truthful and ensure kg only overwrites or deletes the
filesystem object the user accepted.

It covers compilation pending-line and newline budget bypasses;
same-size/coarse-mtime/missing-file save conflicts; `write-file` destination
overwrite prompting; save-time TOCTOU reduction and directory durability;
unchecked `read()`/capacity handling in insert-file; and Dired target
replacement between confirmation and deletion.

## Verification status (checked against source 2026-07-30)

| Claim | Status | Evidence |
| --- | --- | --- |
| Retained newlines are not charged against the cap | Confirmed | `src/compile.c:444` appends `"\n"` unconditionally while `s->stored_output += to_add` counts only the line body; a newline-only stream appends rows forever with `stored_output` stuck at 0 |
| An unterminated long line grows memory without bound | Confirmed | `compilation_append_char()` (`src/compile.c:409-423`) doubles `pending_line_cap` with no reference to `maximum_output`; nothing charges pending bytes until the line commits |
| The cap is 8 MiB and is set at start | Confirmed | `src/compile.c:235` |
| Draining/reaping continue after truncation | **Already true** | `compilation_poll()` (`:555`) always reads and always `waitpid()`s; invariant 2 below needs a regression, not a fix |
| Disk-change detection misses deletions and stat errors | Confirmed | `file_state_differs()` (`src/fileio.c:37-45`) returns 0 — "unchanged" — on any `stat()` failure |
| It misses same-size/same-second rewrites | Confirmed | compares `st_mtime` (second granularity) and `st_size` only |
| Snapshot fields are duplicated three times | Confirmed | `src/def.h:317-319` (`editor_config`), `:417-419` (`editor_buffer`), `:786-787` (`temp_load_result`); copied field-by-field in `src/bufmgr.c:78-80` and `:120-122` |
| `write-file` can adopt an existing destination silently | Confirmed, refined | `editor_write_file()` (`src/fileio.c:517`) sets `editor.filename` (`:537`) **before** calling `editor_save()`. The new name is not special, so `editor_save()` skips its correct "File %s exists. Overwrite?" prompt (`:457-462`) and instead asks the wrong question — "File %s changed on disk. Save anyway?" — comparing the destination against the *previous* file's snapshot (`:476`). When the destination happens to share size and mtime-second with that snapshot there is no prompt at all |
| `insert-file` mishandles `read()` | Confirmed | `src/fileio.c:590` stores into `size_t n`, tests `(ssize_t)n < 0` afterwards, never retries `EINTR`, and `bufcap = (buflen + n) * 2 + 1` (`:605`) is unchecked. The `else if (!buf)` branch (`:615-625`) is unreachable: `buflen + n < bufcap` implies `bufcap > 0` implies `buf != NULL` |
| `insert-file` narrows `size_t` to `int` | Confirmed | `undo_push(..., char *text, int len)` (`src/def.h:990`) and `editor_insert_text_raw(const char *, int)` (`src/buffer.c:998`) both receive `buflen` |
| Dired deletion is a TOCTOU | Confirmed | `dired_delete_flagged()` (`src/dired.c:510`) rebuilds a path string from row text and calls `dired_remove()` (`:497`), which `lstat()`s and then `rmdir()`/`unlink()`s that path. No identity is captured at flag or confirm time at all |

## Read first

- `src/compile.c`, `src/compile.h`, `test/test_compile.c`
- `src/fileio.c`; injection seams `editor_write_fn`/`editor_fsync_fn`/
  `editor_close_fn`/`editor_rename_fn` (`src/fileio.c:48-51`,
  `src/def.h:797-800`) and their mocks in `test/test_buffer.c:1226-1437`
- `src/def.h:317-319`, `:417-419`, `:786-787`
- `src/bufmgr.c:78-122` (snapshot propagation), `:274-325` (`autorevert_poll()`)
- `src/dired.c`, `test/test_dired.c`, `test/stubs_buffer.c`
- `src/shell.c` for the bounded capture pattern

`test/test_buffer.c`, `test/test_minibuf.c` and `test/test_dired.c` all link
`fileio.o`, `bufmgr.o`, `dired.o` and `compile.o` (`EXTRA_buffer`,
`EXTRA_minibuf`, `EXTRA_dired` in the `Makefile`), so file-identity tests
already have a home.

## Invariants

1. Every retained compilation byte, including newlines and pending fragments,
   counts against one total budget.
2. Hitting a retention cap never stops pipe draining or child reaping.
3. A normal save replaces only the disk identity/version the user accepted.
4. "Unable to check" is never "unchanged".
5. `write-file` never overwrites an existing destination without an explicit
   destination-exists decision.
6. Dired deletes the object confirmed by the user, or refuses because it
   changed.

## Phase 1 — Make compilation budgeting testable

Files: `src/compile.h`, `src/compile.c`, `test/test_compile.c`.

### Changes

The byte parser is **already** parameterized: `compilation_process_bytes(struct
compilation_state *s, const char *bytes, size_t len)` (`src/compile.c:474`)
touches no globals except through `s`, and `test/test_compile.c:76-129` already
models the compilation buffer with stubbed `buf_prepare_special_text()`,
`buf_append_special_text()` and `buf_truncate_last_row()` writing into a flat
`g_model`. Do **not** introduce a `compilation_feed()` + emit-callback layer;
it duplicates a seam that exists.

Three things are missing:

1. `compilation_process_bytes()` is `static`. Drop `static` and declare it in
   `src/compile.h`, noting it is exposed for tests.
2. `maximum_output` is hard-coded at its one call site (`src/compile.c:235`).
   Add `void compilation_set_maximum_output(size_t bytes)`, storing it in a
   file-static default that `compilation_start()` copies into
   `g_compilation.maximum_output`, so a running compilation is unaffected.
3. Add `compilation_stream_reset(struct compilation_state *s, size_t
   maximum_output)` zeroing exactly the streaming fields (`pending_line*`,
   `stored_output`, `truncated`, `truncation_marker_written`, `ansi_state`,
   `pending_cr`, `displayed_pending_length`), so a test can build a state on the
   stack without forking and `compilation_start()` stops repeating that list
   inline (`:234-250`).

Use `size_t` throughout and the existing `checked_add_size_t()` /
`checked_mul_size_t()` helpers (`src/def.h:605-621`). Preserve ANSI/CR/BS
handling byte-for-byte.

Pitfall: `compilation_process_bytes()` calls `buf_truncate_last_row()` and
`buf_append_special_text()` on entry and exit (`:481`, `:552`), so a stack-local
state must still name a valid `compilation_buffer` index — use `0`, which is
what the test file's `buf_prepare_special_text()` returns.

## Phase 2 — Enforce one total retained-byte budget

Files: `src/compile.c`, `test/test_compile.c`.

### Changes

In a comment above the budget fields in `src/compile.h`, state exactly what
counts: every byte stored in `pending_line`; every newline retained in
`*compilation*` (the fix — `src/compile.c:444` charges nothing today); the
truncation marker's bytes; and **not** the fixed header written by
`compilation_start()` (`:265-268`) or the trailing status line, which are
editor-generated and bounded. Say so explicitly, because the tests assert
against the budget.

Behavior:

1. retain until the remaining budget is zero;
2. reserve the marker's length up front so it always fits, and append it
   exactly once (`truncation_marker_written` already exists, `:620-627`);
3. after truncation, keep running the state machine so CR/ANSI/backspace state
   stays correct, but neither grow `pending_line` nor append rows;
4. keep reading the pipe and reaping the child;
5. report the final exit status even when output was truncated.

`compilation_append_char()` must respect the remaining budget: cap
`pending_line_cap` at `min(geometric growth, maximum_output - stored_output +
slack)`, drop bytes past it and set `truncated`. Never allocate a full oversized
line and then trim it.

### Tests (`test/test_compile.c`)

Drive `compilation_process_bytes()` directly against a stack state built by
`compilation_stream_reset(&s, 16)`, then assert on `g_model` and on
`s.pending_line_cap` / `s.stored_output`. Add a helper that returns
`g_model_len` so the tests can assert total retained bytes.

- exact boundary, one byte below, one byte above;
- a newline-only stream of 10 000 bytes: retained bytes must stop at the cap
  and `truncated` must be set — the primary regression;
- one 1 MiB unterminated line fed in 4 KiB chunks: `pending_line_cap` must
  never exceed the documented bound;
- `\r`/`\n`, an ANSI CSI, and an OSC each split across a chunk boundary;
- the truncation marker appears exactly once even as the stream continues;
- EOF before child exit and child exit before EOF — the end-of-run path lives in
  `compilation_poll()`, so keep the live-subprocess style of
  `test_streaming_no_final_newline()` for that one;
- `realloc` failure leaves the state consistent (`compilation_append_char()`
  returns silently today — decide whether that sets `truncated`, and document
  the answer).

## Phase 3 — Introduce a richer disk snapshot

Files: `src/def.h`, `src/fileio.c`, `src/bufmgr.c`, `test/test_buffer.c`.

### Changes

Collapse the three duplicated field groups into one struct and give the
comparison three outcomes:

```c
struct file_snapshot {
	dev_t device;
	ino_t inode;
	off_t size;
	struct timespec mtime;
	struct timespec ctime;
	bool existed;
	bool valid;   /* false == never successfully stat()ed */
};

enum file_change_state {
	FILE_SAME,
	FILE_DIFFERENT,
	FILE_UNKNOWN
};

int file_snapshot_path(const char *path, struct file_snapshot *out);
int file_snapshot_fd(int fd, struct file_snapshot *out);
enum file_change_state file_snapshot_compare_path(
    const char *path, const struct file_snapshot *accepted);
```

Classification: a file that existed and now gives `ENOENT`, a changed
device/inode, or a changed size/mtime/ctime → `FILE_DIFFERENT`; any other
`stat()` error (`EACCES`, `EIO`, `ELOOP`, …) or `!accepted->valid` →
`FILE_UNKNOWN`; everything equal → `FILE_SAME`.

Replace the field triples at `src/def.h:317-319`, `:417-419` and `:786-787`
with one `struct file_snapshot` each, and the field-by-field copies in
`src/bufmgr.c:78-80` / `:120-122` with a struct assignment. Keep
`editor.disk_changed` as the display flag (`src/display.c:397` reads it) but
feed it from the enum.

Use `st_mtim`/`st_ctim` (`struct timespec`) behind a feature check; where the
platform only offers `st_mtime`, document the weaker comparison in a comment
and lean on inode/ctime.

`autorevert_poll()` (`src/bufmgr.c:274`) must treat `FILE_UNKNOWN` as "do not
silently revert" — today `file_state_differs()` returning 0 on a stat error is
what makes an unreadable file look clean.

Pitfall: `file_state_differs()` (declared `src/def.h:806`, two callers) must be
deleted, not wrapped — a leftover boolean API invites new "unable to check ==
unchanged" callers.

## Phase 4 — Separate conflict policy from destination-exists policy

Files: `src/fileio.c` (`editor_save()` :434, `editor_write_file()` :517),
`test/test_buffer.c`, new `test/pty/` write-file cases.

### Changes

Extract the correct prompt that already exists inside `editor_save()`'s
special-buffer branch (`src/fileio.c:454-462`) into a helper both paths call:

```c
/* Returns 1 to proceed.  Prompts only when `path` names an existing
 * object that is not the exact file this buffer accepted. */
static int confirm_destination_exists(
    int fd, const char *path, const struct file_snapshot *accepted);
```

`editor_write_file()` then reads the new name (`editor_read_line_path()`,
unchanged), calls `confirm_destination_exists()` **before** touching
`editor.filename`, reports `Save aborted` and returns with the buffer untouched
on refusal, and only then swaps in the new name/syntax and saves. On failure or
cancel it must restore the old name, syntax **and snapshot** together — the
current restore (`:541-544`) omits the snapshot, so a failed Save As leaves the
buffer holding another file's metadata.

`editor_save()`, non-special path: `FILE_DIFFERENT` → the existing "changed on
disk. Save anyway?" prompt; `FILE_UNKNOWN` → `Cannot verify %s on disk. Save
anyway? (y/n)`, defaulting to no; `FILE_SAME` → proceed; special/new buffer →
destination-exists policy via the shared helper. Never answer "does this
destination already exist?" with metadata equality.

### Tests

Native, in `test/test_buffer.c` beside the existing `editor_write_rows_to_file`
mocks: two files with identical size and mtime but different bytes compare
`FILE_DIFFERENT` (inode differs); a hard link to the accepted inode compares
`FILE_SAME`; a destination unlinked after the snapshot compares
`FILE_DIFFERENT`; a `stat()` denied by chmodding the parent to 0 compares
`FILE_UNKNOWN` (return early when running as root, as `test_dired.c` guards do).

PTY, `backend: tmux` so the prompt text can be asserted:

- `write-file-existing-destination-prompts`: plant a second file via
  `config_files`, `C-x C-w` onto it, answer `n`; the original file must be
  unchanged and the mode line must still name it
  (`expected_screen_contains: ["exists"]`);
- `write-file-cancel-restores-name`: same, then `C-x C-s` must land in the
  original file;
- `write-file-accept-adopts-name`: answer `y`; assert the destination's content
  and that the mode line names it.

The path prompt is the completing picker: a bare `RET` on a directory descends
into it, and `M-RET` accepts the typed text literally (see
`test/pty/dired-listing.yaml`'s header comment).

## Phase 5 — Reduce save TOCTOU and make rename durable

Files: `src/fileio.c` (`editor_write_rows_to_file()` :77), `src/def.h`
(injection seams), `test/test_buffer.c`.

### Changes

Open the parent directory once and operate relative to it: derive the basename
safely from the resolved target; use `openat()`/`fstatat()`/`renameat()` where
available, guarded by `#if defined(AT_FDCWD)`; create the temp file in that same
directory (already the case, `:128-136`); immediately before the rename,
`fstatat()` the destination name and compare against the accepted snapshot,
aborting and unlinking the temp when identity changed unless the user forced
this exact later state; `fsync()` the temp before rename (already done, `:172`);
`fsync()` the parent directory fd after a successful rename — the comment at
`:168-171` says directory sync is skipped, so update it rather than leave it
contradicting the code. Keep a portable fallback with the same state model and a
comment naming the residual race.

**Preserve current symlink behavior**: `lstat()` the target, and if it is a
symlink, `realpath()` it and write the *target* (`:100-105`). kg replaces the
pointed-to file, not the link. Pin that with a test before restructuring —
`openat`-relative code makes it easy to flip accidentally.

### Pitfalls

- `editor_rename_fn` has the signature `int (*)(const char *, const char *)`
  (`src/fileio.c:51`). Moving to `renameat()` changes the seam and breaks
  `mock_rename_error` in `test/test_buffer.c:1417`. Either keep a
  `rename`-shaped seam that internally uses `renameat` with a stored dirfd, or
  update the mock in the same commit — the tests will fail to compile
  otherwise, which is the desired loud failure.
- Add a `editor_pre_rename_hook_fn` (default NULL) so a test can replace the
  destination between the final `fstatat()` and the `renameat()`
  deterministically. That is the only way to make the race a regression test
  rather than a race.
- `fsync()` on a directory fd fails with `EINVAL` on some filesystems; treat
  that as success, not as a failed save.

## Phase 6 — Fix insert-file size/error handling

Files: `src/fileio.c` (`editor_insert_file()` :557), `test/test_buffer.c`.

### Changes

- store `read()` into `ssize_t n` and test `n < 0` before any conversion; retry
  on `EINTR` instead of aborting; convert to `size_t` only once `n >= 0`;
- grow with `checked_add_size_t()` / `checked_mul_size_t()` instead of
  `bufcap = (buflen + n) * 2 + 1` (`:605`), and delete the unreachable
  `else if (!buf)` branch (`:615-625`);
- before inserting, require `checked_size_to_int(&len_int, buflen)` to succeed,
  because both `undo_push()` and `editor_insert_text_raw()` take `int`;
- on any error or OOM, free the staging buffer and leave the buffer and undo
  stack untouched — the function already returns before `undo_push()` on those
  paths; keep that ordering when restructuring.

Add a `ssize_t (*editor_read_fn)(int, void *, size_t)` seam beside the existing
four in `src/fileio.c:48-51` and `src/def.h:797-800` so the sequences below are
injectable rather than requiring exotic files.

### Tests

Native, in `test/test_buffer.c`: short reads, `EINTR` then data, error after
partial data, a length that trips `checked_size_to_int()`, an empty file (must
report `(empty file)` and not push undo), and a file whose only content is
`"\n"` (the trailing-newline strip at `:639-641` must not underflow).

## Phase 7 — Revalidate Dired deletion targets

Files: `src/dired.c`, `test/test_dired.c`,
`test/pty/dired-flagged-delete*.yaml`.

### Changes

Split the current `dired_delete_flagged()` (`src/dired.c:510`) into a collect
step and a verified-delete step, and give both external linkage so they are
unit-testable — `editor_confirm_yn()` is stubbed to always answer *no* in
`test/stubs_buffer.c:49`, so `dired_do_flagged_delete()` itself cannot be
driven natively today.

```c
struct dired_target {
	char name[NAME_MAX + 1];
	dev_t device;
	ino_t inode;
	mode_t type;      /* st_mode & S_IFMT */
};

/* Collect D-flagged rows, stat()ing each relative to `dirfd`.  Returns the
 * count, or -1. */
int dired_collect_flagged(int dirfd, struct dired_target *out, int max);

/* Delete one collected target, refusing when identity changed.  Returns 0,
 * or -1 with errno set (ENOENT when gone, ESTALE when replaced). */
int dired_delete_verified(int dirfd, const struct dired_target *target);
```

`dired_do_flagged_delete()` then: `open(dir, O_RDONLY | O_DIRECTORY |
O_CLOEXEC)` once so the directory stays pinned for the whole operation;
`dired_collect_flagged()`, capturing identity **before** the prompt; confirm,
showing the count as now; `dired_delete_verified()` per target; report deleted,
failed and skipped-because-changed separately; close the dirfd and revert.

`dired_delete_verified()` does `fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW)`,
requires `st_dev`/`st_ino`/`S_IFMT` to match, then `unlinkat(dirfd, name,
S_ISDIR(type) ? AT_REMOVEDIR : 0)`. Retain the deliberate refusal to delete a
non-empty directory: `unlinkat(..., AT_REMOVEDIR)` returns `ENOTEMPTY`, which
the existing failure message already renders (`test/pty/dired-flagged-delete-
failure.yaml` expects `Directory not empty` — check that string still matches
after the change, and update the case if `errno` differs on the target
platform).

Never rebuild an absolute path for the final action; `dired_join()` stays for
display and for `dired_find_file()` only.

Note the listing marks a symlink-to-directory with a `/` suffix
(`dired_read()` uses `path_is_dir()`, i.e. `stat`), while deletion must use
`lstat`/`AT_SYMLINK_NOFOLLOW` semantics and unlink the link. Capturing
`S_IFMT` at collect time makes that explicit instead of implicit.

### Tests

Native (`test/test_dired.c`, which already builds real temp trees with
`make_tree()`/`drop_tree()`): `dired_delete_verified()` succeeds on an
unchanged file; fails and leaves the file when the name now holds a different
inode; fails when a file became a directory or the reverse; fails `ENOENT` on a
vanished name; unlinks a symlink-to-directory without touching its target; and
fails `ENOTEMPTY` on a non-empty directory.

PTY: the existing `dired-flagged-delete.yaml` and
`dired-flagged-delete-failure.yaml` must still pass unchanged — the
behavior-preservation gate. For the race itself, `compilation_poll()` runs while
a minibuffer prompt blocks (`src/tty.c:429-436`; see the comment at
`src/compile.c:295-299`), so a case can `M-x compile` a `sleep 1; rm f; mkdir f`
command, press `x`, wait, answer `y`, and assert the skipped-because-changed
message. Mark it timing-sensitive with an explicit `key_delay` and treat the
native tests as the primary regression: if it proves flaky under the sanitizer
lanes, drop it rather than paper over it with delays. Hostile/control-character
filenames are Plan 01's rendering problem; only do not make them worse.

## Documentation

- `doc/kg.1:1258` currently reads "Output is capped at 8~MiB; truncated output
  is noted in the buffer." Say what counts toward the cap (all retained output
  bytes, line terminators included) and that the process keeps running and is
  still reaped after truncation.
- `README.md` and `doc/kg.1`: the save-conflict wording gains the "cannot
  verify" case, and `C-x C-w` gains the destination-exists prompt.
- `doc/TODO.md` if guarded rename or directory fsync is unavailable on a
  supported platform; `AGENTS.md` only if the new `editor_read_fn` / pre-rename
  hook establish a general injection convention. No keybindings change, so
  `src/help.c` is untouched.

## Commit sequence

1. Expose the compilation stream seams (no behavior change) + reset helper.
2. Enforce the total retained-byte budget, with the newline and long-line
   tests.
3. Add `struct file_snapshot` / `enum file_change_state` and migrate the three
   duplicated field groups.
4. Fix `write-file` destination policy and `editor_save()` conflict policy.
5. Guarded relative-directory commit and directory fsync.
6. Fix `insert-file` types and growth.
7. Harden Dired deletion.

Phases 3 and 4 must land together in effect but may be two commits; do not ship
3 alone, since it changes what `autorevert_poll()` decides.

## Acceptance

```sh
make check
make WITH_LISP=0 clean all check
.ci/run-ci-steps.sh --parallel
```

Iterate on one case with
`python3 utils/pty_accept.py --kg src/kg --jobs 1 test/pty/<case>.yaml`, and on
one gate with `.ci/ci-04-clang-asan-ubsan.sh`.

Manual checks: compile `yes | tr -d '\n'` and confirm RSS stays flat and kg
responsive; compile `yes ''` and confirm the newline-only stream is capped;
externally replace, then separately delete, an open file and confirm each save
requires an explicit decision; replace a flagged Dired entry after confirming
and confirm kg refuses it.

## Budget warning

`make complexity-check` caps total scc complexity at 4208 (`Makefile:143`) and
per-file at 520; the tree measures 4171 total today, with `fileio.c` at 126,
`compile.c` at 162 and `dired.c` at 115. The per-file caps are not the problem;
the **37 points of total headroom shared by all thirteen plans** is. Phases 1,
3 and 7 all replace branchy inline code with helpers, so aim for a net-neutral
or negative delta per commit and record the before/after numbers in the PR.
`make pmccabe-check` caps a single function at 120; the worst here is
`compilation_poll` at 25, so splitting its end-of-run block out is optional, not
required.
