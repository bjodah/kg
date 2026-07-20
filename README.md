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
- Multiple buffers with shared kill ring
- Split-window support
- Visual mark mode: the region renders in reverse video as you move
- Per-buffer mark ring: C-u C-SPC jumps to the mark and pops older
  marks; C-y, M-< and M-> push a mark like in Emacs
- Shift-select and the CUA clipboard trio (Shift-Delete / Ctrl-Insert
  / Shift-Insert) alongside the Emacs C-w / M-w / C-y
- Rectangle commands (C-x SPC, C-x r {k,y,d,c,t})
- Smart-case literal and regexp search; query-replace (M-% / ESC %)
- Multi-level undo (C-_)
- Paragraph reflow to 72 columns (M-q)
- Keyboard macros (C-x ( / C-x ) / C-x e; C-u N C-x e repeats N times)
- M-x, C-x C-f, and C-x b all share an ido-style picker: substring
  matching, already-open files pushed to the back of the file picker
- Detects external changes to open files; optional auto-revert
- Shell commands (M-!) and pipe-region-through-command (M-|)
- Comment-dwim (M-;)
- Git commit mode: `COMMIT_EDITMSG` buffers get comment dimming, a
  column-50 subject warning, `C-c C-c` to commit and `C-c C-k` to
  abort; `C-x #` finishes any `$EDITOR` session
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

Extension packages load explicitly with `(kg-load "name")`, which resolves a
bare name to `<config>/kg/lisp/name.fe` and treats names containing `/` as
literal paths. Packages may load other packages; loading a file twice
evaluates it twice (there is no require/provide). Init files and packages are
trusted code with the full privileges of the editor process, bounded only by
the evaluation step budget and `C-g` cancellation.

The init file can also toggle editor options by running named commands,
e.g. enabling electric bracket pairing (off by default):

```lisp
(kg-command "electric-pair-mode")
```

Packages can define interactive commands and bind them to keys:

```lisp
(kg-define-command "insert-date" (fn () (kg-insert "2026-07-04")))
(kg-bind-key "C-c d" "insert-date")
```

Lisp-defined commands appear in `M-x` completion and run under the same
step budget and error recovery as `eval-expression`; `kg-remove-command`
and `kg-unbind-key` undo the registrations. Only `C-c <key>` sequences
are bindable — `C-c` is reserved for user bindings, so they can never
shadow built-in keys.

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
