[![2][]][1] [![4][]][3] [![6][]][5] <a href="https://www.flaticon.com/free-icons/kg" title="Kg icons created by alekseyvanin - Flaticon"><img src="doc/kg.png" width=100 align="right"></a>

# Light Weight UTF-8 Terminal Text Editor

A spiritual descendant of [mg][mg] (Micro Emacs) — same Emacs heritage,
about half the lines of code.  See mg's README for the wider
Emacs-in-a-terminal family kg comes from.

kg is a small, fast terminal text editor with pure Emacs keybindings.
Suitable for editing system files or quick fixes on remote systems where
a full GUI editor is not available.

With syntax highlighting for many languages, multiple buffers, split
windows, incremental search, and multi-level undo, kg punches above its
weight while requiring no external runtime libraries — no curses, just
standard VT100 escape sequences.

## Features

<a href="doc/screenshot.png"><img align="right" src="doc/screenshot.png" width=360 title="kg in action"></a>

- Pure Emacs-style keybindings
- Syntax highlighting for many programming languages, including
  hex/binary/octal integer literals
- Multiple buffers sharing one kill ring, which holds one entry (there
  is no M-y yank-pop yet)
- Split-window support
- Visual mark mode: the region renders in reverse video as you move.
  The mark, the mark ring and the active region belong to the buffer,
  not to the window: two windows showing one buffer share one region,
  and switching buffers leaves each buffer's region as you left it
- Per-buffer mark ring: C-u C-SPC jumps to the mark and pops older
  marks; C-y, M-< and M-> push a mark like in Emacs
- M-@ marks the next word without moving point; repeat it (or add C-u N)
  to grow the region a word at a time
- Shift-select and the CUA clipboard trio (Shift-Delete / Ctrl-Insert
  / Shift-Insert) alongside the Emacs C-w / M-w / C-y
- Rectangle commands (C-x SPC, C-x r {k,y,d,c,t})
- Smart-case literal and regexp search; query-replace (M-% / ESC %),
  confined to the region when one is active
- Search history: M-p/M-n inside an incremental search recall earlier
  search strings, and C-s (or C-r) with an empty query repeats the last
  search; literal and regexp searches keep separate rings, like Emacs
- Minibuffer history: M-p/M-n (also Up/Down, C-p/C-n) recall earlier
  input at the shell-command, query-replace, compile, Eval, goto-line and
  string-rectangle prompts.  Each prompt has its own ring, except the four
  query-replace prompts, which share one
- Multi-level undo (C-_)
- Paragraph reflow to 72 columns (M-q)
- Keyboard macros (C-x ( / C-x ) / C-x e; C-u N C-x e repeats N times)
- M-x, C-x C-f, and C-x b all share an ido-style picker: substring
  matching, already-open files pushed to the back of the file picker;
  M-x RET with nothing typed repeats the last M-x command.  In the path
  picker, `M-RET` submits the typed text literally instead of applying
  the completion (an intentional deviation from Emacs), and so does
  `RET` when the last component is `.` or `..`, so `C-x C-f . RET`
  opens the prompt's directory in dired the way Emacs does
- Detects external changes to open files by identity, not just by
  timestamp: a replaced, removed or unexaminable file is flagged and a
  save over it asks first; optional auto-revert reloads only what can
  still be read back. `C-x C-w` asks before it overwrites an existing
  destination
- Shell commands (M-!) and pipe-region-through-command (M-|); a prefix
  argument inserts/replaces with the output instead of just displaying it;
  both entry points recall previous commands from one shared history.  The
  child runs in its own process group, so whatever it starts is signalled
  with it; kg blocks until it exits
- Comment-dwim (M-;)
- Git commit mode: `COMMIT_EDITMSG` buffers (and `MERGE_MSG` etc.,
  matched on the exact basename) get comment dimming, a column-50
  subject warning, `C-c C-c` to commit and `C-c C-k` to abort; `C-x #`
  finishes any `$EDITOR` session
- Git rebase mode: `git-rebase-todo` buffers (matched on the exact
  basename) highlight actions, commit hashes, `exec` bodies and
  comments, with typoed actions and invalid flags in warning color;
  `C-c C-p/C-r/C-e/C-s/C-f/C-d` set pick/reword/edit/squash/fixup/drop
  on the current line, `M-p`/`M-n` move it up/down, `C-c C-c` continues
  the rebase and `C-c C-k` aborts it
- Dired mode: `M-x dired`, `C-x d`, or opening a directory (`C-x C-f`,
  or `kg somedir`) lists it in a read-only buffer; `RET` visits the
  entry at point, `^` goes up, `g` re-reads, `n`/`p` move, `m` marks
  with `*`, `d` flags with `D`, `u` clears either, and `x` deletes the
  flagged entries after one confirmation (never recursively)
- YAML mode: `.yaml`/`.yml` files highlight keys, comments, quoted and
  block scalars (`|`/`>`), booleans/null, numbers, and structural
  markers; `M-x yaml-mode` enables it manually
- Word-case bindings (M-u / M-l / M-c)
- Transpose chars (C-t)
- Space cleanup (M-\ / M-SPC) and zap-to-char (M-z)
- Sort lines in the region (M-x sort-lines, single-step undo)
- Open line (C-o) and join-line (M-^)
- Quoted-insert (C-q) for literal Tab/Esc/control bytes
- Universal-argument (C-u / M-0..M-9) for repeated commands, capped at
  1000
- Auto-indent; electric bracket pairing via `M-x electric-pair-mode`
  (off by default, can be enabled from the init file)
- Suspend to background (C-z)
- Built-in help in a scrollable *help* buffer (C-h)
- Compilation: `M-x compile` / `M-x recompile` running the buffer's
  `compile-command` in `/bin/sh -c` asynchronously, output streaming into a read-only
  `*compilation*` buffer; cancel with `M-x kill-compilation` or `C-c C-k` inside `*compilation*`
- File-local and directory-local variables (limited, non-evaluating
  `-*- ... -*-` modeline, `Local Variables:` footer, and a safe
  `.dir-locals.el` subset) for `compile-command` and `buffer-read-only`
- `read-only-mode` (`C-x C-q`) with buffer-local state and an `RO`
  mode-line indicator
- No dependencies (not even curses)
- Uses standard VT100 escape sequences
- Tab stops every 8 columns, like Emacs' default `tab-width`
- Display columns measured the way the terminal draws them: East-Asian-Wide
  and Fullwidth characters take two columns, combining marks none (Unicode
  15.1 width table, no libc locale required)
- Nothing kg reads becomes something the terminal obeys: bytes from a
  file, a filename, a directory listing, a Lisp string or a subprocess
  are always drawn as characters. Control characters show as `^X`, and
  C1 controls and invalid UTF-8 as `\xnn`; see [kg(1)][7]
- Graceful terminal resize handling
- Local-variable and `.dir-locals.el` parsing is non-evaluating and
  works in `WITH_LISP=0` builds; see [kg(1)][7] for the exact
  supported subset

## Usage

```
kg [-QRVh] [file ...]
```

| Option | Description                  |
|--------|------------------------------|
| `-Q`   | Do not load the Lisp init file |
| `-R`   | Open file(s) read-only       |
| `-V`   | Print version and exit       |
| `-h`   | Print this help and exit     |

Multiple files can be opened at once, each in its own buffer.  See the
[man page][7] for more in-depth information as well as the full key
binding reference.

## Building and Installing

kg is written in C23; building requires GCC 14+ or Clang 19+ (any
compiler accepting `-std=c23`).

```bash
git submodule update --init --recursive # required for Fe and tiny-regex-c
make
sudo make install          # installs to /usr/local/bin and /usr/local/share/man/man1
```

Lisp support is compiled in by default. Use `make WITH_LISP=0` to build without
the Fe Lisp interpreter; the nested `fe/tiny-regex-c` submodule is still needed
for editor regexp search. `kg -V` reports `+lisp` or `-lisp` for the selected
configuration.

Override the prefix or use DESTDIR for staged installs:

```bash
make install prefix=/usr
make install DESTDIR=/tmp/pkg
```

To uninstall:

```bash
sudo make uninstall
```

## Lisp

Use `M-x eval-expression` (bound to `M-:`) to evaluate Lisp entered in the minibuffer,
`M-x eval-buffer` to evaluate the current buffer, or `C-j` to evaluate the
s-expression before point (in `*scratch*` and Lisp Interaction/Lisp buffers
`C-j` inserts the result; elsewhere it behaves as a plain newline). Results and
labelled errors are shown in the status area. A build made with `WITH_LISP=0`
keeps all commands available and reports that Lisp was not compiled in.

On startup kg loads `$XDG_CONFIG_HOME/kg/init.fe` (falling back to
`~/.config/kg/init.fe`); a missing file is normal, and `-Q` skips loading
entirely so a broken configuration can be repaired. Load errors show the
labelled diagnostic in the status area; forms evaluated before the error
remain applied.

Extension packages load explicitly with `(load "name")`, which resolves a
bare name to `<config>/kg/lisp/name.fe` and treats names containing `/` as
literal paths. Packages may load other packages; loading a file twice
evaluates it twice (there is no require/provide). Init files and packages are
trusted code with the full privileges of the editor process, bounded only by
the evaluation step budget and `C-g` cancellation.

Buffer positions use Emacs' convention: a position is a 1-based codepoint
offset, so `(point-min)` is 1, `(point-max)` is one past the last character,
and every line break counts as one character. Offsets count characters, not
bytes, so multi-byte text addresses the same way it reads.

| Form | Result |
| ---- | ------ |
| `(point)` | Position of point |
| `(point-min)` / `(point-max)` | Buffer bounds |
| `(goto-char N)` | Move point to `N`, clamped to the buffer |
| `(goto-line N)` | Move point to the beginning of line `N`, clamped to the buffer |
| `(line-number-at-pos)` | 1-based line of point |
| `(current-column)` | Display column of point (tabs expand, wide characters count two) |
| `(mark)` | Position of the mark, or `nil` |
| `(set-mark N)` | Set the mark at `N` and activate the region |
| `(deactivate-mark)` | Drop the region highlight, keep the mark |
| `(region-beginning)` / `(region-end)` | Region bounds; an error with no mark |
| `(buffer-substring BEG END)` | Text between two positions, in either order |
| `(char-after)` / `(char-after N)` | Codepoint as a number, `nil` at end of buffer |
| `(forward-word)` / `(forward-word N)` | Move point over `N` words |
| `(backward-word)` / `(backward-word N)` | Move point back over `N` words |
| `(bounds-of-thing-at-point THING)` | Cons `(START . END)` for `'word` or `'line`, or `nil` |

`goto-line` counts lines from 1 and, like its Emacs namesake, takes no
column: reach one by moving on from the beginning of the line with
`(goto-char (+ (point) N))`.

`bounds-of-thing-at-point` returns a cons cell, so `car` and `cdr` read the
two positions just as they do in Emacs, and both things are bounded the way
Emacs bounds them. `'word` names the word containing point: point immediately
after a word still belongs to that word, and point between two words yields
`nil`. `'line` runs from the start of the line to the start of the next one,
so it includes the line break; on the last line it ends at `(point-max)`.
Any other symbol is an error rather than a silent `nil`.

Its word constituents include every codepoint from U+0080 up, so `héllo` and
`漢字` come back whole. This is the one place that is true: the interactive
word commands (`M-f`, `M-b`, `M-@`, `M-d`) are ASCII-only and stop at the
first accented character, and so are the Lisp `forward-word` and
`backward-word`, which drive the same editor primitives.

Upstream fe has no string operations, so kg registers its own. They index by
codepoint like the position API, so no result is ever cut mid-glyph. Almost
all of the Emacs Lisp surface is bought this way — as kg natives and as the
prelude below — rather than in the `fe/` submodule; `doc/fe-upstream.md`
lists the few changes that did have to be made there, and why.

| Form | Result |
| ---- | ------ |
| `(string-length S)` | Length of `S` in characters, not bytes |
| `(substring S FROM)` / `(substring S FROM TO)` | 0-based character indices; negative counts from the end |
| `(concat A B ...)` | Joins any number of strings; `(concat)` is `""` |
| `(string= A B)` | `t` when the strings are equal, else `nil` |
| `(char-to-string N)` | One-character string for codepoint `N` |
| `(string-to-char S)` | First codepoint of `S` as a number, `nil` for `""` |
| `(format FORMAT ARG ...)` | Substitutes `%s`, `%S`, `%d`, `%e`, `%f` and `%g`; `%%` is a literal per cent |

`substring` clamps out-of-range indices instead of signalling, and a `TO`
before `FROM` yields `""`. `char-to-string` rejects 0, surrogates and values
above `U+10FFFF` so the result is always well-formed text; it is the inverse
of `char-after`, which returns a number.

`format` takes the four specifiers Emacs Lisp reaches for most. `%s` prints
an object the way the interpreter prints it — a string bare, a list as a
list, `nil` as `nil` — and `%S` is the same with strings quoted; `%d`
truncates a number toward zero. There are no field widths, precisions or
flags, and no `%c`, `%x`, `%o`, `%e` or `%f`. Extra arguments are ignored,
as in Emacs, while a missing argument, an unknown specifier and a format
string ending inside one are all errors. Every number is a double, so `%d`
prints the exact integer value of one, which for `1e19` is the same
`10000000000000000000` Emacs prints from a bignum; NaN and the infinities
have no integer to print, so `%d` refuses them where Emacs writes `nan` and
`inf`.

kg also evaluates a prelude at startup, written in Fe, so the Emacs Lisp
surface is available before any init file runs. It is what makes an `init.fe`
read like an `init.el`.

| Group | Forms |
| ---- | ------ |
| Definitions | `defun` `defmacro` `defvar` `defconst` `interactive` `lambda` |
| Binding | `(let ((VAR VALUE) ...) BODY...)` `let*` `(setq VAR VALUE ...)` `progn` |
| Control | `cond` `when` `unless` `prog1` `(dolist (VAR LIST [RESULT]) BODY...)` `(dotimes (VAR COUNT [RESULT]) BODY...)` |
| Lists | `length` `nth` `nthcdr` `last` `reverse` `append` `mapcar` `assoc` `member` `memq` `push` `pop` `caar` `cadr` `cddr` `1+` `1-` |
| Predicates | `null` `eq` `equal` `listp` `type-of` `stringp` `symbolp` `numberp` `consp` `functionp` `boundp` |
| Quoting | `quasiquote`, written `` ` `` with `,` and `,@`; `#'f` is plain `f` |
| Editor | `(string-empty-p S)` and `(thing-at-point THING)` — the text of `(bounds-of-thing-at-point THING)`, or `nil` when there are no bounds |

Argument lists take `&optional` and `&rest`; a missing argument is `nil` and an
extra one is dropped. `length` also counts the codepoints of a string. `equal`
is structural on lists, where Fe's `is` compares pairs by identity.

A name that has never been assigned is an error rather than `nil`, so a typo
says `void-variable NAME` instead of quietly being false. `(boundp 'NAME)` asks
whether a name has a value, `(makunbound 'NAME)` takes it away, and a variable
holding `nil` is bound — which is what `defvar` tests before initialising.

```lisp
(defun initialise (words)
  "Insert the initial of every non-empty word in WORDS."
  (dolist (word words)
    (unless (string-empty-p word)
      (insert (concat (substring word 0 1) ". ")))))

(initialise (list "alpha" "beta"))
```

Where it differs from Emacs Lisp, and these are worth knowing before the
first surprise:

- `=` is assignment, not numeric comparison — use `setq` to assign and `eq`
  or `is` to compare.
- `eq` compares numbers and strings by value, so `(eq "a" "a")` is `t` where
  Emacs says `nil`. Only pairs are compared by identity.
- Every number is a double, and there is no character type: write
  `(string-to-char "a")` rather than `?a`.
- `t` is an ordinary assignable global.
- There is no `unwind-protect` or `condition-case`, no dynamic binding, no
  vectors or hash tables.
- Recursion is bounded at roughly 450 frames by the interpreter's
  garbage-collector stack, so walk long lists with `while`, not recursion.
- A structure that refers to itself prints as far as the cycle and then
  `#<cycle>`, rather than being printed forever.

The editor bridge uses the Emacs names throughout: `insert`, `message`,
`buffer-name`, `load`, `global-set-key` and `global-unset-key`. `message`
formats, so `(message "%s at %d" name (point))` reaches the status area with
its arguments substituted, and a literal per cent in a message has to be
written `%%`.

The init file can also toggle editor options by running named commands,
e.g. enabling electric bracket pairing (off by default):

```lisp
(command-execute 'electric-pair-mode)
```

`command-execute` runs one of the built-in editor commands kg allows Lisp to
call, named by a quoted symbol as in Emacs or equivalently by a string, and
always without a prefix argument.

Which commands those are, and which of them a read-only buffer refuses, is
one table in the editor — the same one M-x and every key binding consult, so
a command cannot be refused by one route and allowed by another. Commands
defined in Lisp count as commands that edit the buffer, so a read-only buffer
refuses them too; there is no way yet for a `defun` to say otherwise.

Packages define interactive commands the way Emacs does, with `defun` plus
`(interactive)`, and bind them by name:

```lisp
(defun insert-date ()
  "Insert today's date."
  (interactive)
  (insert "2026-07-04"))
(global-set-key "C-c d" "insert-date")
```

An `(interactive)` body form is stripped from the function and registers it
under its own symbol. The registry underneath is reachable directly as
`(define-command NAME FUNCTION)`, which takes a symbol or a string, and
`remove-command` undoes it.

A worked `init.fe` — select the word under the cursor, the way you would
write it in Emacs:

```lisp
(defun select-current-word ()
  "Select the word under point, or say there is none."
  (interactive)
  (let ((bounds (bounds-of-thing-at-point 'word)))
    (if bounds
        (progn (goto-char (car bounds))
               (set-mark (cdr bounds)))
      (message "No word found at point."))))
(global-set-key "C-c w" "select-current-word")
```

`C-c w` now selects the word under point, so `C-w` kills it, `M-w` copies it
and `C-x C-x` bounces between its ends. One detail differs from Emacs:
`set-mark` already activates the region, so there is no separate
`activate-mark` to call.

Lisp-defined commands appear in `M-x` completion and run under the same
step budget and error recovery as `eval-expression`; `remove-command`
and `global-unset-key` undo the registrations. Only `C-c <key>` sequences
are bindable — `C-c` is reserved for user bindings, so they can never
shadow built-in keys. A mode that defines its own `C-c` keys (git commit
and rebase buffers, `*compilation*`) shadows the user's binding of that
same sequence while it is current, and every other `C-c` key still
reaches the user's.

## Development

Before submitting changes, format the C sources and tests:

```bash
make format
make format-check
```

The formatter is `clang-format` with the repository's WebKit-based
`.clang-format` profile.  CI runs `make format-check`, so formatting-only
drift is caught there as well.

Useful local quality checks:

```bash
make check
make fuzz-keypress
make complexity-check
make pmccabe-check
make coverage
make compile-db
make iwyu
```

`make iwyu` runs Include What You Use from the compilation database, so
refresh `compile_commands.json` with `make compile-db` first.

For crash triage and fuzzing notes, see [doc/FUZZING.md](doc/FUZZING.md).

## Origin & References

kg is based on [kilo][0] by Salvatore Sanfilippo (antirez), the original
minimal text editor that demonstrates how to build a functional editor
without dependencies in about 1000 lines of C code.

The name "kg" is a nod to [mg][mg] (Micro Emacs), suggesting "kilo-gram"
— a minimal implementation with Emacs keybindings.  mg's README is the
place to read up on the broader lineage if the heritage matters to you.

[0]:  https://github.com/antirez/kilo
[mg]: https://github.com/troglobit/mg
[1]: https://en.wikipedia.org/wiki/BSD_licenses
[2]: https://img.shields.io/badge/License-BSD%202--Clause-green.svg
[3]: https://github.com/troglobit/kg/actions/workflows/build.yml/
[4]: https://github.com/troglobit/kg/actions/workflows/build.yml/badge.svg
[5]: https://github.com/troglobit/kg/releases
[6]: https://img.shields.io/github/v/release/troglobit/kg?include_prereleases
[7]: https://man.troglobit.com/man1/kg.1.html
