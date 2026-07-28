# TODO In Priority Order

## Missing Mg features

Features and keybindings present in Mg but missing from kg, roughly
ordered by value vs implementation effort.

### High value, straightforward

- [x] **M-% query-replace**: Interactive find-and-replace.  Prompt for
      search string then replacement, step through matches with y/n/!
      (replace all)/q.  The incremental search machinery in search.c is
      a natural starting point.

- [x] **C-x C-w write-file**: Save buffer to a different filename (Save
      As).  Prompts for a new name, writes, and updates the buffer's
      filename.

- [x] **C-x i insert-file**: Insert the contents of a file at point.

- [x] **M-; comment-dwim**: Toggle or insert a line comment using the
      current buffer's syntax.  Requires plumbing comment-start strings
      from the syntax table through to an editing command.

- [x] **C-x C-x exchange-point-and-mark**: Swap cursor and mark.  Lets
      you visually inspect the other end of a region or bounce between two
      positions.  Trivial to implement.

- [x] **C-o open-line**: Insert a newline at point without advancing the
      cursor.  The classic "make room above the next line" command.  One
      liner.

- [x] Visual mark mode

- [x] Adjust filename in bar, gets cut off if the path is too long.
      Same issue applies to long buffer names; need a sensible ellipsis
      strategy (e.g. shorten leading path components like Emacs does).

- [x] Crash when opening doc/TODO.md

        ~/src/kg(master)$ kg doc/TODO.md 
        malloc(): invalid next size (unsorted)
        Aborted (core dumped)
- [x] Let C-x C-f use the directory of the current buffer as starting point.


### Stability / safety (high priority)

- [x] **`M-!` shell-command** and **`M-|` shell-command-on-region**:
      Run an external shell command, optionally piping the current region
      through it and replacing the region with the output.  For a remote-shell
      editor this is the highest-leverage missing feature — pipe through
      `sort`, `fmt`, `column -t`, `jq`, `sed`, etc.  Mg has it.

- [x] **Tab completion in the minibuffer**: For `C-x C-f`, `C-x i`,
      `C-x C-w` and any other path-prompting command.  Single biggest
      day-to-day friction once everything else works.

- [x] **External-modification detection on save**: When editing
      `/etc/*` something else may have rewritten the file.  Stat the
      file before save; if mtime/size changed since we read it, prompt
      "file changed on disk, save anyway?".  Also: live `(changed)`
      marker in the mode line, plus optional `M-x auto-revert-mode` /
      `global-auto-revert-mode` that silently reload clean buffers.

- [ ] **Backup-on-save / simple autosave**: Write a `file~` (or `#file#`)
      safety net on first save in a session.  Cheap to add, saves grief
      when ssh drops mid-edit.

- [x] **`C-q` quoted-insert**: Insert the next keystroke literally.
      Currently there is no way to insert a literal Tab, Esc, or other
      control byte — matters for terminfo, sendmail.cf, Makefiles.

### Medium value

- [x] **M-x named commands**: Execute a command by name.  Opens the door
      to toggles (auto-fill, overwrite-mode) without burning key bindings.
      Significant infrastructure but makes kg extensible.

- [x] **Keyboard macros  C-x ( / C-x ) / C-x e**: Record and replay a
      sequence of keystrokes.  Even a single-level macro (no nesting)
      covers the vast majority of real use.  F3 and F4 added as aliases
      for start/stop/execute.

- [x] **M-x revert-buffer**: Re-read the current file from disk, discarding
      all unsaved changes (with confirmation prompt if dirty).  Useful after
      an external tool modifies the file.

- [x] **M-^ join-line**: Join current line with the previous one
      (complement to the C-k-at-EOL join-with-next that kg already has).
      Also add `join-line` as an M-x command.

- [x] **M-u / M-l / M-c  upcase / downcase / capitalize word**: Operate
      on the word from point forward.  word.c already walks words; just
      add tolower/toupper passes.  Also add `upcase-word`, `downcase-word`,
      and `capitalize-word` as M-x commands.

- [x] **Git commit mode (and the wider EDITOR-server crowd)**: When kg
      runs as `GIT_EDITOR` — filename matches `COMMIT_EDITMSG`,
      `MERGE_MSG`, `TAG_EDITMSG`, etc. — enter a small dedicated mode
      with Emacs-style `C-c C-c` to finalize (save and exit 0) and
      `C-c C-k` to abort (exit non-zero so git cancels the commit
      with an empty message).  `C-x #` does the same as `C-c C-c`
      for the broader EDITOR-server flow that crontab, sudoedit, hg,
      and friends share.  Pair with syntax highlighting that dims
      comment lines (`#` prefix), warns past column 50 on the
      subject, and a `M-q` that respects column 72 in the body but
      leaves the subject alone.  Lands kg in the "what's your
      $EDITOR" conversation alongside `emacs -nw` and `vim`.
      Implemented as a syntax-flag-detected mode (`SHL_GITCOMMIT`), no
      Lisp package; see
      [w7b-git-commit-mode.md](file:///work/.meta-docs/plans/w7b-git-commit-mode.md).

### Lower priority / larger scope

- [x] **C-u universal-argument**: Numeric prefix for repeating commands
      (e.g. C-u 8 C-f moves forward 8 chars).  Moderate plumbing work
      but unlocks power-user workflows.

- [x] **M-z zap-to-char**: Kill from point up to and including a
      prompted character.

- [ ] **M-y yank-pop**: After C-y, cycle backwards through the kill ring
      with repeated M-y presses.  Requires expanding the kill ring from a
      single slot to a small ring (Emacs default is 60; even 8 would cover
      most use).

- [ ] **Multi-entry kill ring**: Prerequisite for M-y yank-pop.  Keep
      the ring bounded (8–16 entries) to avoid unbounded memory growth.

- [x] **M-SPC just-one-space**: Collapse whitespace around point to a
      single space.  Also add `just-one-space` as an M-x command.

- [x] **C-t transpose-chars**: Swap the character before point with the
      one at point.  Indispensable for fixing the most common typing errors.
      Also add `transpose-chars` as an M-x command.

- [ ] **M-t transpose-words**: Companion to `transpose-chars`.

- [x] **Rectangle operations**: `C-x r k` kill-rectangle, `C-x r y`
      yank-rectangle, plus `C-x r d` delete and `C-x r c` clear.
      `C-x r t` string-rectangle is still TODO.  Surprisingly useful
      for config tables, fstab columns, CSV-ish data.

- [ ] **Registers / bookmarks**: At minimum position registers via
      `C-x r SPC` (point-to-register) and `C-x r j` (jump-to-register).
      Text registers (`C-x r s` / `C-x r i`) can follow.

- [ ] **Regex search**, or at least a case-sensitivity toggle in
      isearch.  Right now isearch is literal-only and case-sensitive.

- [x] **Verify `M-d` kill-word forward and `M-DEL` kill-word backward**:
      Pair with `M-f`/`M-b` that already exist; add if missing.

- [ ] **Toggle line numbers**: `M-x linum-mode` or similar.  Frequent
      ask, low cost.

- [x] **Minimal config file**: `~/.config/kg/init.fe` — done; see the Lisp
      section in README.md (init files, `(load ...)`, `define-command`,
      `global-set-key`). What exists now:
      - an Emacs-shaped position and mark API (`point`, `goto-char`,
        `goto-line`, `point-min`/`point-max`, `line-number-at-pos`,
        `current-column`, `mark`, `set-mark`, `deactivate-mark`,
        `region-beginning`/`region-end`,
        `buffer-substring`, `char-after`, `forward-word`/`backward-word`,
        `bounds-of-thing-at-point`), addressing the buffer by 1-based
        codepoint offsets
      - string natives (`string-length`, `substring`, `concat`, `string=`,
        `char-to-string`, `string-to-char`), which upstream fe lacks
      - an Emacs Lisp prelude evaluated at startup: `defun`, `defmacro`,
        `defvar`, `defconst`, `interactive`, `let`/`let*` with elisp binding
        lists, `setq`, `progn`, `cond`, `when`, `unless`, `prog1`, `dolist`,
        `dotimes`, `quasiquote`, the list library (`length`, `nth`,
        `nthcdr`, `last`, `reverse`, `append`, `mapcar`, `assoc`, `member`,
        `memq`, `push`, `pop`), `equal`, `string-empty-p`,
        `thing-at-point`; `&optional` and `&rest` in argument lists
      - type natives `type-of`, `stringp`, `symbolp`, `numberp`, `consp`,
        `functionp`
      - Emacs names throughout the editor bridge (`insert`, `message`,
        `buffer-name`, `load`, `global-set-key`, `global-unset-key`,
        `command-execute`); the only invented names are `define-command`
        and `remove-command`, which Emacs has no analogue for — and
        `(interactive)` inside a `defun` writes to that registry, so they
        rarely need to be typed
      - the fe submodule carries elisp `if`, `lambda` as the primitive's
        name, the `` ` ``/`,`/`,@`/`#'` reader macros, `void-function`
        errors and a 4096-slot GC stack; see `doc/fe-upstream.md`

      Remaining Lisp follow-ups:
      - `=` still means assignment rather than numeric comparison.  Making
        it numeric is a one-line prelude change but silently changes the
        meaning of any stale `(= x v)`, so it wants its own release
      - no docstring registry / `documentation`; docstrings are inert
      - no dotted unquote (`` `(a . ,b) ``) and no nested quasiquote
      - editor option variables (`tab-width`, `auto-fill-column`, ...) exposed
        to Lisp; still only commands/bindings and the editing bridge exist
      - grow the `command-execute` allow-list deliberately (policy per
        command).  It is still the eleven entries in `allowed_commands`
        in `src/lisp.c`
      - **word constituents disagree between the two layers**: the global
        `is_word_char()` in `src/word.c` is ASCII-only (`isalnum() || '_'`),
        so `M-f`, `M-b`, `M-@`, `M-d` and the Lisp `forward-word` /
        `backward-word` that drive them stop inside "héllo", while the Lisp
        `bounds-of-thing-at-point` treats every codepoint from U+0080 up as
        a word constituent and returns the whole word. Deliberate for now —
        making interactive word motion codepoint-aware is part of the
        "Proper UTF-8 handling" item below, not a Lisp change
      - extend the keypress fuzz harness to drive `eval-expression` once it
        can run without filesystem side effects
      - upstream Fe: `FeCallWithOptions` so hosts can budget a bare `FeCall`
        without the run trampoline; Fe plan phase 8 (per-context custom
        types) remains deferred

## Important (DONE)

- [x] Refactor code to Linux style, variable decl. at top of context sorted
      in reverse chrismas tree style, lines can be up to 110 chars long
- [x] Add hash-bang fallback for detection of syntax highlighting, e.g., #!/bin/sh
- [x] Fix delete key
- [x] Add auto-indent à la Mg (consider M-x auto-indent-mode toggle or
      C-j vs Enter distinction)
- [x] Add built-in help, similar in style to whay my fork of Mg has, see
      ~/src/mg/ for details
- [x] Add markdown-mode with syntax highlighting
- [x] Add support for multiple buffers, supporting copy-paste between
      them, i.e., shared kill ring
- [x] Add support for split windows, both horizontal and vertical
- [x] Change the mode line to be more similar to Emacs, the current
      active window marker '**' is so easily mistaken for "aha a modifed
      buffer", we could instead use ansi escape sequences to set all the
      non-selected windows as "dim"
- [x] With two buffers open, we should only need to ask "are you sure"
      if any buffer is modified and not saved when exiting
- [x] Testing and stability to reach "usable" level
- [x] Add support for Emacs' M-q to "reflow" a paragraph

## Maybe Later

- [ ] Send alt screen sequences if TERM=xterm: "\033[?1049h" and "\033[?1049l"
- [ ] Add support for modes.  E.g., c-mode with bindings for
      compile/make which opens a compile buffer in a window below
- [ ] **Proper UTF-8 handling**.  `editor.cx` and `mark_col` are byte
      indices, so the cursor can sit inside a multi-byte glyph and
      Right/Left advances one byte at a time through box-drawing or
      accented characters.  The visual-mark renderer now defers its
      reverse-video toggle to glyph boundaries (display-time fix), and
      display columns now go through the width table in `src/width.c`,
      but region extraction, word motion, and the syntax classifier
      (`syntax.c`, where `!isprint()` on high-bit bytes tags every byte
      of a UTF-8 glyph as `HL_NONPRINT` and substitutes `?`, so CJK in a
      file with a syntax renders as a row of `?`) still treat each byte
      on its own.  A real fix means walking by `utf8_glyph_span_at()`
      everywhere we step through chars.

- [ ] **Partial combining-mark coverage in the width table**.
      `src/width.c` encodes the full Unicode 15.1 East_Asian_Width W/F
      set but only a curated slice of the nonspacing marks: the generic
      combining blocks plus Hebrew, Arabic, Syriac, Thaana, NKo,
      Samaritan, Mandaic, Thai, Lao, conjoining Hangul jamo, Mongolian
      and the variation selectors.  The Brahmic scripts (Devanagari and
      the rest of the Indic blocks, Tibetan, Myanmar, Khmer, Balinese)
      and the musical/mathematical marks above U+FE2F still measure one
      column each.  Grapheme clustering is also out of scope: an emoji
      ZWJ sequence or a regional-indicator flag pair measures as the sum
      of its parts, so a flag reports four columns where the terminal
      draws two.

- [ ] **Horizontal-scroll + tab units mismatch in display.c**.
      `editor.coloff` is used both as a chars-byte offset
      (`editor.coloff + editor.cx == filecol`) and as a render-byte
      offset (the row-render loop indexes `r->render + coloff`).  These
      agree only for rows with no tabs.  Once a row is long enough to
      scroll horizontally and contains a tab, the cursor placement and
      the rendered viewport disagree.  Pre-existing, no user reports;
      fix would unify on render-col units throughout the cursor maths.

- [ ] **mandoc -T lint nits in doc/kg.1**.  Three pre-existing
      "new sentence, new line" warnings (e.g. around `M-a/M-e`'s
      `Mr.\&` and `e.g.\&`).  Cosmetic; mdoc convention says each
      sentence should begin on its own line.
