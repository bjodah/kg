# W7-B Implementation Plan — Git commit mode

> Target audience: a junior developer new to this codebase.
> Parent plan: [wishlist-implementation-plan.md](file:///work/.meta-docs/plans/wishlist-implementation-plan.md) (W7, Option B).
> Design source: [doc/TODO.md L98-L109](file:///work/doc/TODO.md#L98-L109).
> Estimated effort: 8–12 hours, split into 7 phases that each leave the
> tree green (`make check` passes after every phase).

## What you are building

When kg is used as `GIT_EDITOR` / `$EDITOR`, git opens files named
`COMMIT_EDITMSG`, `MERGE_MSG`, `TAG_EDITMSG`, etc.  kg should recognize
these files and provide:

1. **Detection**: a "Git commit" mode, auto-enabled by filename, shown in
   the mode line.
2. **Highlighting**: `#` comment lines dimmed (cyan, like other
   comments); characters past column 50 on the *subject line* shown in
   red as a warning.
3. **Finalize**: `C-c C-c` saves the buffer and exits kg with status 0
   (git proceeds with the commit).
4. **Abort**: `C-c C-k` exits kg *without saving* and with a **non-zero
   exit status** (git cancels the commit).
5. **Server-done**: `C-x #` = save current buffer and exit 0, in *any*
   buffer (the generic EDITOR flow used by crontab, sudoedit, hg, ...).
6. **Reflow policy**: `M-q` refuses to reflow the subject line and
   comment paragraphs; body paragraphs reflow at column 72 (which is
   already kg's `FILL_COLUMN`).

Out of scope (do **not** build these):

- No Lisp package (`git-commit.fe`) and no new Lisp primitives. This is
  a pure C-side feature. The optional Lisp packaging from the parent
  plan is dropped.
- No `core.commentChar` support — assume `#`.
- No special handling of `git commit -v` verbose diffs below the
  scissors line, and no `git-rebase-todo` support (different format).
- No UTF-8 column-exact width for the col-50 warning; byte offsets are
  fine (consistent with the rest of syntax.c).

## Read these first

- `AGENTS.md` — build/test workflow, style rules (C23, tabs,
  kernel-style braces, short helpers).
- `src/syntax.c:655-747` — `markdown_syntax()`: the pattern for a
  custom per-line highlighter, including the `hl_oc` re-trigger idiom.
- `src/kbd.c:234-390` — `editor_process_keypress()` prefix handling
  (`cc_prefix`, `cx_prefix`).
- `utils/pty_accept.py` — the PTY acceptance harness you will extend.
- `test/test_syntax.c` — the native syntax-test harness you will extend.

## Key design decisions (already made — follow them)

| Decision | Rationale |
|---|---|
| Detection rides on the existing syntax machinery: a new `HLDB` entry with a `SHL_GITCOMMIT` flag. "Is this a commit buffer?" = `editor.syntax && (editor.syntax->flags & SHL_GITCOMMIT)`. | Single source of truth. The syntax pointer is already saved/restored per buffer (`editor_buffer.syntax`, def.h:361), already re-selected on open and on save-as (`fileio.c:187`), and its `name` already appears in the mode line. No new per-buffer state needed. |
| Filename match uses the existing `filematch` substring rule. | `syntax.c:1289-1300`: non-dot patterns match anywhere in the path, so `.git/COMMIT_EDITMSG` matches pattern `COMMIT_EDITMSG` with zero parser changes (same trick as the existing `Makefile` entry). |
| New highlight value `HL_WARNING` (red) for the subject overflow. | `HL_MATCH` (blue) and `HL_COMMENT` (cyan) have other meanings. `row->hl` is a byte array; adding a value costs one `switch` case in `editor_syntax_to_color()`. |
| `C-c C-c` / `C-c C-k` are **built-in** interceptions in kbd.c, active only in commit buffers. | `keybind_parse()` (keybind.c:45) deliberately refuses `C-c C-c` for user bindings, so it cannot go through the user binding table. In commit buffers the built-in `C-c C-k` shadows any user binding for that sequence — document this. In all other buffers user bindings behave exactly as before. |
| Exit status is a new global `kg_exit_status` (default 0) returned by `main()`. | kg currently always returns 0 from `main()` (main.c:147); every quit path funnels through `running = 0`. A global keeps the normal shutdown path (`kg_lisp_shutdown()`, `atexit` terminal restore) intact — do **not** call `exit()`. |
| `C-x #` works in every buffer, not just commit buffers. | Per TODO.md: it serves the generic EDITOR-server flow. |
| `M-q` skips subject and comment paragraphs entirely rather than trying to be clever. | Predictable, safe, and `FILL_COLUMN` is already 72 for the body — nothing else to change in word.c. |

---

## Phase 1 — Exit-status plumbing

Tiny, self-contained, needed by phases 3–4.

1. `src/def.h` — next to `extern int running;` (~L380), add:

   ```c
   extern int kg_exit_status; /* Process exit status returned by main(). */
   ```

2. `src/main.c` — next to `int running = 1;` (main.c:44), add
   `int kg_exit_status = 0;`.  At the end of `main()` (main.c:147),
   change `return 0;` to `return kg_exit_status;`.

Nothing sets it yet, so behavior is unchanged. Build with `make` and
run `make check`.

## Phase 2 — Syntax: detection + highlighting

### 2a. Constants (`src/def.h`)

- After `#define HL_MATCH 8` (def.h:100) add:

  ```c
  #define HL_WARNING 9 /* Overlong commit subject, etc. */
  ```

- After `#define SHL_MAKEFILE (1 << 3)` (def.h:104) add:

  ```c
  #define SHL_GITCOMMIT (1 << 4) /* Git commit message highlighter. */
  ```

- In the `/* syntax.c */` declarations block of def.h, declare:

  ```c
  [[nodiscard]] int syntax_is_git_commit(void);
  [[nodiscard]] int syntax_git_commit_subject(void);
  ```

### 2b. HLDB entry (`src/syntax.c`)

Near `MAKE_HL_extensions` (syntax.c:559) add:

```c
/* Git message files: matched as substrings anywhere in the path, so
 * .git/COMMIT_EDITMSG works (see the filematch rule at the top). */
char *GITCOMMIT_HL_extensions[] = { "COMMIT_EDITMSG", "MERGE_MSG",
	"SQUASH_MSG", "TAG_EDITMSG", "NOTES_EDITMSG", "EDIT_DESCRIPTION",
	NULL };
```

**Append** a new entry at the **end** of `HLDB[]` (after the Lisp entry,
~syntax.c:614). Do not insert it in the middle: `test/test_syntax.c`
indexes `HLDB` by position (e.g. `HLDB[19]` = Markdown), so appending
keeps existing tests valid.

```c
	{ "Git commit", GITCOMMIT_HL_extensions, NULL, "", "", "",
	    SHL_GITCOMMIT },
```

The mode line will now show `Git commit` automatically for these files.

### 2c. The highlighter (`src/syntax.c`)

Add near `markdown_syntax()`:

```c
#define GITCOMMIT_SUBJECT_LIMIT 50

/* True when the current buffer is a git commit/merge/tag message. */
int syntax_is_git_commit(void)
{
	return editor.syntax && (editor.syntax->flags & SHL_GITCOMMIT);
}

/* Row index of the commit subject: the first non-blank row that is not
 * a '#' comment.  Returns -1 when the buffer has no subject yet. */
int syntax_git_commit_subject(void)
{
	int j;

	for (j = 0; j < editor.numrows; j++) {
		erow *r = &editor.row[j];

		if (r->rsize == 0 || r->render[0] == '#') {
			continue;
		}
		return j;
	}
	return -1;
}

/* Git commit message highlighter.  Dims '#' comment lines and warns in
 * red past column 50 on the subject line.  hl_oc stores "this row is
 * the subject" so edits that move the subject re-trigger neighbours,
 * mirroring the markdown fence idiom. */
static void gitcommit_syntax(erow *row)
{
	int oc = 0;

	if (row->render[0] == '#') {
		memset(row->hl, HL_COMMENT, row->rsize);
	} else if (row->idx == syntax_git_commit_subject()) {
		oc = 1;
		if (row->rsize > GITCOMMIT_SUBJECT_LIMIT) {
			memset(row->hl + GITCOMMIT_SUBJECT_LIMIT, HL_WARNING,
			    row->rsize - GITCOMMIT_SUBJECT_LIMIT);
		}
	}

	/* If this row gained or lost subject status, re-highlight the
	 * next non-empty row so the warning follows the subject.  Blank
	 * rows are skipped: editor_update_syntax() returns early for
	 * them and never updates their hl_oc. */
	if (row->hl_oc != oc) {
		int j;

		for (j = row->idx + 1; j < editor.numrows; j++) {
			if (editor.row[j].rsize > 0) {
				editor_update_syntax(&editor.row[j]);
				break;
			}
		}
	}
	row->hl_oc = oc;
}
```

Notes for the implementer:

- `editor_update_syntax()` already memsets `row->hl` to `HL_NORMAL` and
  returns early for empty rows before your code runs (syntax.c:907-921),
  so `gitcommit_syntax` may assume `rsize > 0` and a clean hl array.
- The forward re-trigger cannot recurse infinitely: it only fires when a
  row's subject status *changes*, and it only walks forward.
- Known accepted staleness: `editor_del_row()` (buffer.c:361) does not
  re-highlight following rows, so deleting the subject line leaves stale
  colors until the next edit. Markdown fences have the same quirk; do
  not fix it here.

### 2d. Dispatch and color

- In `editor_update_syntax()` after the `SHL_MAKEFILE` dispatch
  (syntax.c:930-933) add:

  ```c
  	if (editor.syntax->flags & SHL_GITCOMMIT) {
  		gitcommit_syntax(row);
  		return;
  	}
  ```

- In `editor_syntax_to_color()` (syntax.c:1147-1166) add:

  ```c
  	case HL_WARNING:
  		return 31; /* red */
  ```

  (Yes, `HL_NUMBER` is also 31; that's fine — numbers are never
  highlighted in commit buffers.) Verify with
  `grep -n 'HL_' src/display.c` that rendering maps *all* hl values
  through `editor_syntax_to_color()` and needs no change.

### 2e. Native unit tests (`test/test_syntax.c`)

Follow the existing makefile/markdown test pattern
(`setup(&HLDB[N])` + `editor_insert_row()` + `CHECK(row->hl[i] == ...)`,
see test/test_syntax.c:188-320). Your entry is the **last** HLDB slot —
count the entries after you append (should be index 21) and add a
comment saying "Git commit" next to the index. Add tests:

1. `# comment line` → every byte `HL_COMMENT`.
2. A 60-char subject on row 0 → bytes 0–49 `HL_NORMAL`, 50–59
   `HL_WARNING`.
3. Comment on row 0 + text on row 1 → row 1 is the subject (insert row 1
   first or re-run `editor_update_row(&editor.row[1])` after both rows
   exist — see the setext test comment at test/test_syntax.c:293-297 for
   why ordering matters).
4. Subject row 0 + 60-char body row 2 (blank row 1) → body row has **no**
   `HL_WARNING`.
5. Buffer of only comments → `syntax_git_commit_subject() == -1`, no
   warning anywhere.

Register the new test functions in that file's `main()` runner the same
way the existing ones are registered. Run `make check`.

## Phase 3 — Finalize / abort / server-done keys (`src/kbd.c`)

### 3a. Helpers

Add two static helpers near `editor_confirm_quit()` (kbd.c:170):

```c
/* Finish an EDITOR-server session (C-c C-c in git-commit buffers,
 * C-x # anywhere): save the current buffer, then quit with status 0.
 * A failed or cancelled save keeps the session running. */
static void editor_server_done(int fd)
{
	if (editor_save(fd) != 0) {
		return;
	}
	if (!editor_confirm_quit(fd)) {
		return;
	}
	running = 0;
}

/* Abort a git commit (C-c C-k): quit without saving and with a
 * non-zero exit status so git discards the commit. */
static void editor_commit_abort(int fd)
{
	int answer;

	editor_set_status_message("Abort commit? (y/n) ");
	editor_refresh_screen();
	answer = editor_read_key(fd);
	if (answer != 'y' && answer != 'Y') {
		editor_set_status_message("");
		return;
	}
	kg_exit_status = 1;
	running = 0;
}
```

Notes:

- `editor_save()` (fileio.c:148) returns 0 on success and already
  handles its own prompting/error messages.
- `editor_confirm_quit()` protects *other* dirty buffers; after a
  successful save the current buffer is clean, so the common
  single-file git flow never prompts.
- The abort prompt is unconditional (even for a clean buffer) so the
  binding is predictable and test-deterministic.

### 3b. C-c interception

Replace the `cc_prefix` block (kbd.c:293-298) with:

```c
	/* Handle C-c <key>: built-in commit-mode keys first, then user
	 * bindings installed with (kg-bind-key ...).  C-c C-c is never
	 * user-bindable (see keybind_parse), and in commit buffers the
	 * built-in C-c C-k shadows any user binding. */
	if (editor.cc_prefix) {
		editor.cc_prefix = 0;
		if (syntax_is_git_commit() && c == CTRL_C) {
			editor_server_done(fd);
		} else if (syntax_is_git_commit() && c == CTRL_K) {
			editor_commit_abort(fd);
		} else {
			handle_user_binding(c, fd);
		}
		return;
	}
```

### 3c. C-x #

In the `cx_prefix` switch (kbd.c:301-385) add, near the other
punctuation cases like `'('` / `')'`:

```c
		case '#': /* C-x #: save and exit 0 (EDITOR-server done) */
			editor_server_done(fd);
			break;
```

Manual smoke test before moving on:

```
$ make
$ printf '\n# comment\n' > /tmp/COMMIT_EDITMSG
$ ./src/kg /tmp/COMMIT_EDITMSG    # type a subject, C-c C-c
$ echo $?                          # expect 0; file saved
$ ./src/kg /tmp/COMMIT_EDITMSG    # type garbage, C-c C-k, y
$ echo $?                          # expect 1; file unchanged
```

## Phase 4 — M-q reflow policy (`src/word.c`)

In `editor_reflow_paragraph()` (word.c:980), right after the paragraph
boundaries are computed (after the `para_end` loop ending at
word.c:1014) and **before** any allocation, add:

```c
	/* Git commit buffers: never reflow the subject line or comment
	 * paragraphs; the body reflows at FILL_COLUMN (72) as usual. */
	if (syntax_is_git_commit()) {
		int subject = syntax_git_commit_subject();

		if (editor.row[para_start].chars[0] == '#') {
			editor_set_status_message(
			    "Not reflowing commit comments");
			return;
		}
		if (subject >= para_start && subject <= para_end) {
			editor_set_status_message(
			    "Not reflowing the commit subject");
			return;
		}
	}
```

Notes:

- `editor.row[para_start].size > 0` is guaranteed here (the function
  already returned for empty rows at word.c:1001).
- If the user hasn't yet separated subject from body with a blank line,
  the subject's paragraph includes the body — skipping the whole
  paragraph is the intended safe behavior.
- `FILL_COLUMN` is already 72 (word.c:9); the body needs no change.

## Phase 5 — PTY harness: `expected_exit_code`

The acceptance harness captures the editor's exit code
(`RunResult.exit_code`, utils/pty_accept.py:55) but never asserts it.
Extend `utils/pty_accept.py`:

1. Add `expected_exit_code: int | None` to the `Case` dataclass.
2. In `load_case()`:
   - parse `expected_exit_code=data.get("expected_exit_code")`,
     validating `isinstance(v, int)` when present;
   - **reject** `expected_exit_code` combined with `backend: tmux`
     (raise `ValueError`): the tmux runner hardcodes exit code 0
     (utils/pty_accept.py:363) because the process runs detached.
3. In `evaluate_case()`, after the saved-file comparison and the screen
   checks, add:

   ```python
   if passed and case.expected_exit_code is not None:
       if kg_run.exit_code != case.expected_exit_code:
           passed = 0
           details = (f"exit code {kg_run.exit_code}, "
                      f"expected {case.expected_exit_code}")
   ```

Keep the style of the file (tabs, same error-message format).

Harness gotcha to remember for phase 6: the default trailer is
`C-x C-s C-x C-c` (utils/pty_accept.py:20). Cases where kg exits from a
key inside `keys:` **must** set `trailer_keys: []`, otherwise the
harness tries to type into a dead PTY and errors out.

## Phase 6 — PTY acceptance tests (`test/pty/`)

YAML cases are picked up automatically by `make check` (Makefile:90).
Use `backend: pexpect` (the default) for all exit-code cases. `C-c` in
key tokens is literal Ctrl-C, `M-q` is ESC+q — see `token_to_bytes()`.

### 6a. `git-commit-finalize.yaml`

```yaml
name: git-commit-finalize
filename: COMMIT_EDITMSG
initial: |
  Add the frobnicator

  # Please enter the commit message for your changes.
keys:
  - C-c
  - C-c
trailer_keys: []
expected_saved: |
  Add the frobnicator

  # Please enter the commit message for your changes.
expected_exit_code: 0
```

### 6b. `git-commit-abort.yaml`

```yaml
name: git-commit-abort
filename: COMMIT_EDITMSG
initial: |
  # Please enter the commit message for your changes.
keys:
  - garbage
  - C-c
  - C-k
  - y
trailer_keys: []
expected_saved: |
  # Please enter the commit message for your changes.
expected_exit_code: 1
```

(The file on disk must be untouched even though the buffer was dirty.)

### 6c. `server-edit-done.yaml` — `C-x #` on a plain file

```yaml
name: server-edit-done
filename: note.txt
initial: |
  hello
keys:
  - X
  - C-x
  - "#"
trailer_keys: []
expected_saved: |
  Xhello
expected_exit_code: 0
```

### 6d. `git-commit-reflow.yaml` — M-q policy

Subject M-q is a no-op; body M-q wraps at 72. With a body of 20 × the
word `word` (single spaces, one line), 14 words fit in 72 columns
(14×5−1 = 69), leaving 6 on the second line:

```yaml
name: git-commit-reflow
filename: COMMIT_EDITMSG
initial: |
  This subject line is deliberately much longer than fifty characters total

  word word word word word word word word word word word word word word word word word word word word
keys:
  - M-q      # on the subject: refused, buffer unchanged
  - C-n
  - C-n      # onto the body line
  - M-q
expected_saved: |
  This subject line is deliberately much longer than fifty characters total

  word word word word word word word word word word word word word word
  word word word word word word
```

(Default trailer saves and quits; no exit-code assertion needed.)
Remove the inline `#` comments if the YAML list parser complains —
check how other cases format comments first.

### 6e. Optional: mode-line check (tmux)

A tmux-backed case asserting the mode line shows the mode:

```yaml
name: git-commit-modeline
filename: COMMIT_EDITMSG
backend: tmux
initial: |
  subject
keys: []
expected_saved: |
  subject
expected_screen_contains:
  - Git commit
```

Do not try to assert colors via escape sequences — too fragile.

## Phase 7 — Documentation

1. **`src/help.c`** — two edits, keeping every line exactly 79 display
   columns (open `*help*` with `C-h` afterwards and eyeball alignment):
   - Put `C-x #` in the empty third-column cell of the `C-j` row
     (help.c:55-56): `│ C-x #    save+exit      │`.
   - Add a footer line after the `C-c <key>` note (help.c:86-87):
     `"  Git commit files: C-c C-c commit (save, exit 0) · C-c C-k abort (exit 1)"`.
2. **`doc/kg.1`** — add a short *Git commit mode* paragraph. Grep the
   man page for how visual-line-mode or the C-c bindings are documented
   and match that structure. Cover: auto-detection filenames, the three
   key bindings, the highlight behavior, the M-q policy.
3. **`README.md`** — one feature bullet, e.g. "Git commit mode:
   `COMMIT_EDITMSG` buffers get comment dimming, a column-50 subject
   warning, `C-c C-c` to commit and `C-c C-k` to abort; `C-x #` finishes
   any `$EDITOR` session."
4. **`doc/TODO.md`** — mark the L98-109 item done (check the box),
   pointing at this plan.
5. **`.meta-docs/plans/wishlist-implementation-plan.md`** — update the
   "Implementation status" section: W7-B implemented, note the actual
   design (syntax-flag detection, no mode.c framework, no Lisp package).

## Final verification

```
make clean && make            # WITH_LISP=1 default build
make check                    # native + PTY suites
make WITH_LISP=0 clean all    # feature must not depend on Lisp
make check                    #   (kbd.c/syntax.c changes are Lisp-free)
.ci/run-ci-steps.sh           # full CI: analyzers, sanitizers, -Werror
```

If a CI stage fails, iterate on that stage directly
(`.ci/ci-01-*.sh` … `.ci/ci-08-*.sh`).

Manual end-to-end sanity check with real git:

```
$ cd $(mktemp -d) && git init -q . && touch f && git add f
$ GIT_EDITOR=/work/src/kg git commit        # C-c C-c after typing → commit created
$ touch g && git add g
$ GIT_EDITOR=/work/src/kg git commit        # C-c C-k, y → "Aborting commit..."
```

## Edge cases checklist (test manually, add tests where cheap)

- [ ] `C-c C-c` when the save fails (e.g. unwritable directory): editor
      stays running, error message shown, exit path not taken.
- [ ] `C-c C-k` then `n` at the prompt: nothing happens, buffer intact.
- [ ] `C-c C-k` in a *non*-commit buffer with a user Lisp binding for
      `C-c C-k`: user binding still runs (interception is gated on
      `syntax_is_git_commit()`).
- [ ] `C-x #` with a second dirty buffer open: `editor_confirm_quit`
      prompt appears; `n` keeps the session alive.
- [ ] Subject exactly 50 bytes: no warning. 51 bytes: one warned byte.
- [ ] Commenting out the subject line (`#` prefix) moves the warning to
      the next non-blank line (the hl_oc re-trigger).
- [ ] `C-x b` away from the commit buffer and back: mode line and
      bindings survive (syntax pointer round-trips through
      `editor_buffer`).
- [ ] `kg -R COMMIT_EDITMSG` (read-only): `C-c C-c` — `editor_save`
      path; verify behavior is sane (save succeeds or errors cleanly),
      `C-c C-k` still aborts.

## Files touched (summary)

| File | Change |
|---|---|
| `src/def.h` | `kg_exit_status`, `HL_WARNING`, `SHL_GITCOMMIT`, 2 syntax.c declarations |
| `src/main.c` | define `kg_exit_status`, return it |
| `src/syntax.c` | filematch array, HLDB entry (appended last), `gitcommit_syntax()`, `syntax_is_git_commit()`, `syntax_git_commit_subject()`, dispatch, color case |
| `src/kbd.c` | `editor_server_done()`, `editor_commit_abort()`, cc_prefix interception, `C-x #` case |
| `src/word.c` | subject/comment guard in `editor_reflow_paragraph()` |
| `src/help.c` | `C-x #` cell + commit-keys footer line |
| `utils/pty_accept.py` | `expected_exit_code` support |
| `test/test_syntax.c` | 5 highlighter unit tests |
| `test/pty/git-commit-*.yaml`, `test/pty/server-edit-done.yaml` | 4–5 acceptance cases |
| `doc/kg.1`, `README.md`, `doc/TODO.md` | documentation |

No Makefile change is needed: no new `.c` files, and PTY YAML cases are
globbed automatically.
