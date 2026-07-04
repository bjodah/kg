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
- Shift-select and the CUA clipboard trio (Shift-Delete / Ctrl-Insert
  / Shift-Insert) alongside the Emacs C-w / M-w / C-y
- Rectangle commands (C-x SPC, C-x r {k,y,d,c})
- Incremental search and query-replace (M-% / ESC %)
- Multi-level undo (C-_)
- Paragraph reflow to 72 columns (M-q)
- Keyboard macros (C-x ( / C-x ) / C-x e)
- M-x, C-x C-f, and C-x b all share an ido-style picker: substring
  matching, already-open files pushed to the back of the file picker
- Detects external changes to open files; optional auto-revert
- Shell commands (M-!) and pipe-region-through-command (M-|)
- Comment-dwim (M-;)
- Word-case bindings (M-u / M-l / M-c)
- Transpose chars (C-t)
- Space cleanup (M-\ / M-SPC) and zap-to-char (M-z)
- Open line (C-o) and join-line (M-^)
- Quoted-insert (C-q) for literal Tab/Esc/control bytes
- Universal-argument (C-u / M-0..M-9) for repeated commands, capped at
  1000
- Auto-indent and bracket autocomplete
- Suspend to background (C-z)
- Built-in help in a scrollable *help* buffer (C-h)
- No dependencies (not even curses)
- Uses standard VT100 escape sequences
- Graceful terminal resize handling

## Usage

```
kg [-RVh] [file ...]
```

| Option | Description                  |
|--------|------------------------------|
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
git submodule update --init # required for the default Lisp-enabled build
make
sudo make install          # installs to /usr/local/bin and /usr/local/share/man/man1
```

Lisp support is compiled in by default. Use `make WITH_LISP=0` to build the
editor without initializing the Fe submodule. `kg -V` reports `+lisp` or
`-lisp` for the selected configuration.

Override the prefix or use DESTDIR for staged installs:

```bash
make install prefix=/usr
make install DESTDIR=/tmp/pkg
```

To uninstall:

```bash
sudo make uninstall
```

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
