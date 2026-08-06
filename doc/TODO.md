# TODO In Priority Order

The dependency-ordered implementation program for the next architecture and
feature work is [doc/plans/2026-07-31-follow-ups](plans/2026-07-31-follow-ups/README.md).
This file remains the broader feature and technical-debt inventory.

## Lisp Interactive Follow-up

- [x] **07E interactive prompting**: `n`/`N`, `s`, `f`/`F`, and `b`/`B` use
      the existing pickers without side effects.
- [ ] **Deferred interactive codes and modifiers**: every valid Emacs code
      kg does not implement (`a b c C d D e G i k K m M R S U v x X z Z`)
      and the three modifiers `*`, `@` and `^` report *unsupported*
      interactive code, not invalid, and do not run the command body.
      Keyboard-macro strings and `CMD_REPEATS` for Lisp commands are
      deferred with them.
- [ ] **Interactive reflection**: `interactive-form` and documentation
      reflection need a metadata decision first — 07D stores the spec
      thunk but not the raw form beside it, so honest reflection has
      nothing to return. `commandp` therefore answers about a *name*
      (the command registry) rather than about a function object, which
      is a recorded divergence: Emacs also calls an anonymous lambda
      carrying an interactive form a command.
- [ ] **Prompt interpolation and `interactive` MODES**: Emacs passes a
      prompt containing `%` through `format` with the earlier interactive
      arguments, and filters commands by mode; kg's prompts are literal
      and the MODES arguments are accepted and ignored. Both are recorded
      divergences in `README.md` and `doc/lisp-api.md`.
- [ ] **Phase 8 read functions**: public `read-string`, `read-number`,
      `read-file-name`, and `read-buffer`, including optional arguments,
      defaults, history, and non-command re-entry semantics.

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
      Implemented as a syntax-flag-detected mode (`SHL_GITCOMMIT`), with no
      Lisp package.

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

- [x] **Registers**: `C-x r SPC` (point-to-register), `C-x r j`
      (jump-to-register), `C-x r s` (copy-to-register), `C-x r i`
      (insert-register).  32 slots, marker-backed positions, bounded text
      (1 MiB per register, 4 MiB total).  Persistent bookmarks deferred.

- [ ] **Regex search**, or at least a case-sensitivity toggle in
      isearch.  Right now isearch is literal-only and case-sensitive.

- [x] **Verify `M-d` kill-word forward and `M-DEL` kill-word backward**:
      Pair with `M-f`/`M-b` that already exist; add if missing.

- [ ] **Toggle line numbers**: `M-x linum-mode` or similar.  Frequent
      ask, low cost.

- [x] **Minimal config file**: `~/.config/kg/init.el` — done; see the Lisp
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
        `char-to-string`, `string-to-char`, `format`), which upstream fe
        lacks; `format` covers widths, `%c`, `%x`, `%X`, `%o` and `%%`, and `message`
        is a format function like its Emacs namesake
      - an Emacs Lisp prelude evaluated at startup: `defun`, `defmacro`,
        `defvar`, `defconst`, `interactive`, `let`/`let*` with elisp binding
        lists, `progn`, `cond`, `when`, `unless`, `prog1`, `dolist`,
        `dotimes`, `quasiquote`, the list library (`length`, `nth`,
         `nthcdr`, `last`, `reverse`, `append`, `mapcar`, `mapc`, `mapconcat`,
         `assoc`, `assq`, `member`, `memq`, `push`, `pop`, `nreverse`, `delq`,
         `delete`, `add-to-list`), `equal`, `string-empty-p`,
         `thing-at-point`, `identity`, `prog2`, `max`, `min`, `defcustom`,
         `custom-set-variables`, `declare`,
         `documentation`, `setq-default`, `setq-local` and `kbd`;
         `&optional` and `&rest` in argument lists
      - type natives `type-of`, `stringp`, `symbolp`, `numberp`, `consp`,
        `functionp`
      - Emacs names throughout the editor bridge (`insert`, `message`,
        `buffer-name`, `load`, `global-set-key`, `global-unset-key`,
        `command-execute`); the only invented names are `define-command`
        and `remove-command`, which Emacs has no analogue for — and
        `(interactive)` inside a `defun` writes to that registry, so they
        rarely need to be typed
      - the fe submodule carries elisp `if`, `lambda` as the primitive's
        name, the `` ` ``/`,`/`,@`/`#'` reader macros (`#'f` reads as
        `(function f)`, and the writer prints one back as `#'f`),
        Lisp-2 namespaces — a separate value and function cell per
        symbol, with call position resolving the function cell only, and
        the nine namespace forms `function`, `funcall`, `apply`,
        `fboundp`, `symbol-function`, `symbol-value`, `fset`, `defalias`
        and `fmakunbound` as core forms rather than prelude
        definitions — `void-function` errors, `unwind-protect`, core
        `setq` (lexical-aware) and `set` (always the global cell),
        left-to-right chained numeric `=`, and a 4096-slot GC stack; and
        from Phase 6, condition objects `(SYMBOL . DATA)` with a static
        hierarchy plus the forms that raise and catch them — `catch`,
        `throw`, `condition-case`, `signal` and `error` as core forms,
        with `ignore-errors` the prelude's macro over the last of them —
        and the host-side completion surface kg's seams are built on:
        `FeGetCompletion`/`FeGetCondition`/`FeGetCompletionMessage`, the
        protected call `FeTryCallWithOptions` and `FeResignal`; see
        `doc/fe-upstream.md`

      Remaining Lisp follow-ups:
      - no call-trace exposure through the host error callback
        (the `call_trace` parameter to `handle_error` is discarded;
        exposing structured call traces is a debugger-shaped feature)
      - re-classification of kg's 112 prose raise sites into named
        condition objects (deferred per 06A Decision 2; most raise
        `(error "text")` which is Emacs' own shape for prose errors).
        Until it lands, `(condition-case e (goto-char "x")
        (wrong-type-argument …))` does *not* match, which
        `test/lisp-compat/features.json`'s
        `condition-case-native-errors` row records as a divergence
      - `save-excursion`/`with-current-buffer` expanded to Lisp
        `unwind-protect` in `lisp/prelude.el`, so a `throw` out of
        either body reaches the `catch` that names its tag instead of
        stopping at Fe's native re-entry wall as `no-catch`
        (`catch-throw-reachability`, the other divergence 06E left)
      - token/cancel cleanup registry (Phase 9's robustness scope;
        currently `unwind-design.md` item 2, belongs to that phase)
      - editor option variables (`tab-width`, `auto-fill-column`, ...) exposed
        to Lisp; still only commands/bindings and the editing bridge exist
      - grow the `command-execute` allow-list deliberately (policy per
        command).  The list is gone: it is the `CMD_LISP_CALLABLE` flag
        on `cmdtable` in `src/cmd.c`, still the same eleven commands,
        and `cmd_invoke()` is the one place that reads it
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
      - upstream Fe: `FeCallWithOptions` (landed) lets hosts budget a bare
        `FeCall`; per-context custom types remain deferred

- [ ] **Dired follow-ups**: dired mode shipped as a C mode (`src/dired.c`)
      with listing, `RET`/`^`/`g`/`n`/`p`, the `*`/`D` markers and `x`
      delete-flagged.  Deliberately left out of that first version:
      - `C` copy and `R` rename the entry (or the marked entries) at point
      - `+` mkdir in the listed directory
      - `U` unmark all, and capital `D` (`dired-do-delete`) acting on the
        `*` marks, which are inert until something consumes them
      - mark-preserving revert: `g` rebuilds the buffer and the markers are
        buffer text, so they are dropped today
      - `ls -l`-style detail columns (size, mode, mtime) and the sort
        toggles that go with them
      Recursive deletion stays out on purpose: `rmdir` failing on a
      non-empty directory is the safety property, not a limitation.

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

- [x] **Horizontal-scroll + tab units mismatch in display.c**.  Fixed:
      `coloff` is a chars-byte offset (`coloff + cx == filecol`), which
      is what every other module already assumed, and the two places
      that read it as a render offset now convert.  The row-drawing
      loop slices `r->render` at `chars_to_render_col(r, coloff)`, and
      the cursor column is the difference of two `editor_visual_col()`
      readings instead of a second, subtly different, walk of the row.
      A scrolled window used to be drawn one tab expansion off and not
      contain point at all
      (`test/pty/horizontal-scroll-tab-slices-render.yaml`).
      See `doc/coordinates.md` row 11b.

- [ ] **mandoc -T lint nits in doc/kg.1**.  Three pre-existing
      "new sentence, new line" warnings (e.g. around `M-a/M-e`'s
      `Mr.\&` and `e.g.\&`).  Cosmetic; mdoc convention says each
      sentence should begin on its own line.

- [x] **Multi-byte input in the minibuffer is broken**.  Fixed: the
      prompt reader dropped every byte above ASCII (`isprint()` rejects
      them), so the search string stayed empty and the following keys
      fell through to the buffer.  Prompts now pull in the rest of the
      UTF-8 sequence (`editor_read_utf8_seq`) and insert it whole, and
      backspace, C-d, C-b/C-f and the echo-area cursor column all work
      in glyphs and display cells instead of bytes.

- [x] **Typing a multi-byte character into the buffer still does
      nothing**.  Fixed: the self-insert arm accepted only TAB and
      32..126, so both bytes of a typed `å` were dropped before they
      reached `editor_self_insert_char`.  A byte above ASCII now
      collects its continuation bytes (`editor_read_utf8_seq`, so
      keyboard macros still replay) and inserts the glyph as one unit
      through `editor_self_insert_glyph`; a malformed sequence is
      dropped rather than half inserted, and read-only buffers refuse
      the lead byte like any other editing key.  The two open decisions
      resolved as: **undo** follows yank, the other command that
      inserts multi-byte text — one `UNDO_YANK_TEXT` record whose
      reverse deletes the glyph's bytes forward, so `C-_` removes the
      whole character and leaves valid UTF-8 (overwrite mode reuses
      `editor_overwrite_char`'s `UNDO_REPLACE_TEXT` record, now with a
      replacement longer than one byte); **`editor.cx`** stays a byte
      offset into `row->chars` and advances by the glyph's byte length,
      which is exactly where `C-f` over the same glyph lands.

- [x] **Regex follow-ups from the Emacs-fidelity work.**
      Both infrastructure halves are done: the differential fuzzer
      against the Emacs oracle is now `make check-regex-differential`
      (`utils/regex_differential.py`, `utils/regex_oracle.el`,
      `test/regex_differential.c`), run in CI by
      `.ci/ci-10-regex-differential.sh`; and the regex fuzz corpus has
      a tracked home in `test/fuzz-seeds/regex`, copied into the
      gitignored working corpus by `make fuzz-regex-seed`.  The one
      non-deliberate divergence they turned up is fixed too, below.
      Known deliberate divergences that remain: `\w` `\d` `\s`, case
      folding and the POSIX classes are ASCII-only (Emacs' `\w`
      matches `å`); a quantifier on a quantifier (`a\{2\}\{2\}`) is
      valid in Emacs but never matches here.
      **Fixed: the capture register after an empty repetition of a
      quantified group.**  `\(x*\|a\)\{2\}b` against `ab` used to give
      group 1 as `1 1` where Emacs gives `0 1`.  The first write-up
      here read it as "Emacs keeps the last iteration that consumed
      anything", and scoped it to `\{n\}`; probing the oracle over the
      whole `\{n,m\}` grid said otherwise on both counts.  Emacs
      compiles a repeat with a non-zero minimum as a counted loop with
      *no* empty-match check, so an empty repetition does not end the
      loop until the minimum is behind it — repetition 1 of
      `\(x*\|a\)\{2\}` matches the empty branch at 0 and repetition 2
      then matches `a`.  Past the minimum the check is back, which is
      why `*`, `\{0,m\}` and `\{n,\}` always agreed, and why enough
      slack over the minimum (`\{2,4\}b` on `ab`) reaches the trailing
      empty repetition again.  `\{n,n+1\}` diverged too and was never
      in the original scope.  `match_rep()` in `fe/tiny-regex-c/re.c`
      now repeats while `done <= min` even on an empty body; kg and
      Emacs agree over 32k hand-built cases spanning the grid, and
      `--cases 200000` agrees on seeds 12 (which used to fail), 13, 14,
      15 and 20260729.  Span 0 was never affected, so only `\1`-style
      references saw it.
