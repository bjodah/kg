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
- Multiple buffers sharing one kill ring, which holds up to 16 entries
  (8 MiB total); C-y yanks the newest, M-y (yank-pop) immediately
  afterward walks older ones
- C-x b to a name no buffer has creates an empty buffer with that name,
  as Emacs' switch-to-buffer does, whatever the name is spelled like.
  It visits no file, so C-x C-s asks where to write it, C-x s never
  offers it, and C-x k kills it without asking about unsaved changes --
  which is also how kg's own `*special*` buffers are killed.
  A buffer you made this way is yours even when it wears one of kg's own
  names: `M-x compile` or a debugger pane will not rebuild a
  `*compilation*` or `*dap-stack*` you typed into, it says whose buffer it
  is and does nothing, and killing yours hands the name back
- Split-window support
- Visual mark mode: the region renders in reverse video as you move.
  The mark, the mark ring and the active region belong to the buffer,
  not to the window: two windows showing one buffer share one region,
  and switching buffers leaves each buffer's region as you left it.
  Marks are persistent positions, so edits before or through them move
  them with the surrounding text, including edits replayed by undo.
- Per-buffer mark ring: C-u C-SPC jumps to the mark and pops older
  marks; C-y, M-< and M-> push a mark like in Emacs
- M-@ marks the next word without moving point; repeat it (or add C-u N)
  to grow the region a word at a time
- Shift-select and the CUA clipboard trio (Shift-Delete / Ctrl-Insert
  / Shift-Insert) alongside the Emacs C-w / M-w / C-y
- Rectangle commands (C-x SPC, C-x r {k,y,d,c,t})
- Registers: `C-x r SPC` saves point in one of 32 registers named by the
  next key, `C-x r j` goes back to it.  A saved position is a persistent
  marker, so edits before it move it with the text; a register whose
  buffer has been killed reports that instead of jumping into whatever
  took the freed slot.  `C-x r s` copies the region into a register and
  `C-x r i` inserts it back as one undo step, byte for byte; registers
  hold 1 MiB each and 4 MiB together, and refuse rather than evict
- Smart-case literal and regexp search; query-replace (M-% / ESC %),
  confined to the region when one is active
- Search history: M-p/M-n inside an incremental search recall earlier
  search strings, and C-s (or C-r) with an empty query repeats the last
  search; literal and regexp searches keep separate rings, like Emacs.
  C-y inside the search appends the newest kill to the query and
  re-searches at once (Emacs' isearch-yank-kill); M-y directly after it
  swaps in the next-older ring entry (isearch-yank-pop-only)
- C-q inside the search quotes the next key into the query
  (isearch-quote-char), so C-s C-q C-j searches for a newline and
  C-q TAB for a literal tab.  A literal query carrying a newline —
  quoted or yanked in — matches across the line break; a regexp query
  is still matched a line at a time
- Minibuffer history: M-p/M-n (also Up/Down, C-p/C-n) recall earlier
  input at the shell-command, query-replace, compile, Eval, goto-line and
  string-rectangle prompts.  Each prompt has its own ring, except the four
  query-replace prompts, which share one.  Prompt kills and yanks
  (C-k, M-d, M-Backspace, C-y, M-y) use the one global kill ring, as
  Emacs' minibuffer does, so text killed in a prompt can be yanked in
  the buffer and the other way around
- Word-level editing in prompts: M-f/M-b move by word, M-d/M-Backspace
  kill by word, M-u/M-l/M-c upcase/downcase/capitalize the word after
  the cursor, and C-q quotes the next byte in.  A prompt's word is
  delimited by whitespace rather than by Emacs' word syntax
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
- Transpose chars (C-t) and words (M-t)
- Space cleanup (M-\ / M-SPC) and zap-to-char (M-z)
- Dynamic abbreviation expansion (M-/, `M-x dabbrev-expand`): the word
  before point is completed from a longer word this buffer already holds,
  nearest occurrence backward from point first, then forward past the word
  point is in.  A second M-/ straight after the first shows the next
  candidate instead of expanding again, each word is offered once however
  often it occurs, and a cycle that runs out puts back what was typed and
  says so.  Any other key ends the cycle, leaving the last expansion.
  Four deliberate differences from Emacs' dabbrev: matching is exact-case
  (Emacs folds case and refits the expansion to the case pattern that was
  typed); a word here is kg's own -- ASCII alphanumerics and `_`, what
  M-f and M-d already move over -- rather than the major mode's syntax
  table, which in a text buffer also joins words with `-`; Emacs
  continues into other buffers once the current one is spent, where kg
  stops at the current buffer; and expanding from the middle of a word,
  Emacs' forward scan can re-offer the word the expansion just created,
  where kg's starts past it.  Reached through `M-x` rather than its key
  it always expands afresh instead of continuing a cycle, which is how
  every kg command that behaves differently when repeated works
- Sort lines in the region (M-x sort-lines, single-step undo)
- Open line (C-o) and join-line (M-^)
- Quoted-insert (C-q) for literal Tab/Esc/control bytes
- Universal-argument (C-u / M-0..M-9) for repeated commands, capped at
  1000
- Auto-indent; electric bracket pairing via `M-x electric-pair-mode`
  (off by default, can be enabled from the init file)
- Show-paren highlighting, **on by default** as in Emacs 28.1 and later:
  with point immediately after a closing bracket, or on an opening one,
  kg colours that bracket and the one it pairs with.  `()`, `[]` and `{}`
  all pair, nesting is counted across all three, and a pair of the wrong
  kind — `(` closed by `]` — is coloured in the mismatch face instead, as
  is a bracket with no partner at all.  Brackets inside a string or a
  comment pair only with each other, so a `)` in a comment never closes
  live code; in a buffer kg has no syntax for, every bracket is code.
  The search gives up after 100k bytes (Emacs'
  `blink-matching-paren-distance`) and reports no partner rather than
  scanning a whole large file on every keystroke.  `M-x show-paren-mode`
  is the way off, and is callable from Lisp, so
  `(command-execute "show-paren-mode")` in an init file turns it off for
  good
- Suspend to background (C-z)
- Mouse support, on by default: click to put point where you clicked (in
  the window you clicked in), drag to select a region, wheel to scroll
  five lines a notch.  kg asks the terminal for SGR mouse reports
  whenever it enters raw mode and takes the request back whenever it
  leaves — quitting, and C-z, which hands the mouse back to the shell and
  takes it again on resume.  There is no capability query worth having, so
  the heuristic is generous: kg asks unless `TERM` is unset, empty,
  `dumb` or `unknown`, and a terminal that does not understand the
  request ignores it.  Clicks on a mode line or the echo area do nothing.

  **Enabling mouse reporting takes the terminal's own selection away**:
  while it is on, dragging selects a kg region instead of marking text
  for the terminal's copy buffer.  Hold **Shift** while dragging to get
  the terminal's native selection back for one drag — every terminal
  worth using keeps Shift for itself.  `M-x xterm-mouse-mode` toggles the
  whole thing off (and on again) if you would rather have the terminal's
  selection all the time; it is also callable from Lisp, so
  `(command-execute "xterm-mouse-mode")` in an init file turns it off for
  good
- Built-in help in a scrollable *help* buffer (C-h)
- Introspection over the command table and the keymaps, rendered into a
  read-only `*Describe*` buffer (`q` closes): `M-x describe-key` reads a
  key sequence and reports the command it runs, the map that answered and
  the binding that map shadows; `M-x describe-command` reports a named
  command's summary, its read-only and Lisp-callability verdicts, and the
  keys that run it; `M-x describe-bindings` lists every binding with the
  map holding it; `M-x where-is` names the keys for a command in the echo
  area.  Every answer is read out of the registries when it is asked, so
  a binding whose Lisp command has been removed reports that rather than
  a stale name
- Compilation: `M-x compile` / `M-x recompile` running the buffer's
  `compile-command` in `/bin/sh -c` asynchronously, output streaming into a read-only
  `*compilation*` buffer; cancel with `M-x kill-compilation` or `C-c C-k` inside `*compilation*`.
  `file:line[:column]:` diagnostics parsed from that output (up to 128 per
  run) are navigable with `next-error`/`previous-error` (`M-g n` / `M-g p`),
  which open or select the source file, in another window when
  `*compilation*` is the selected one, and track the diagnostic's position
  through edits made after visiting it. A recompile discards the previous
  run's diagnostics and restarts the cursor from the first one
- `M-x occur` lists every line of the buffer matching a regexp in a
  read-only `*Occur*` buffer, shown in another window while the source
  stays visible: a header, then `%7d:` and the line's own text per
  matching line, with every match highlighted.  `RET` visits the
  occurrence on the row point is on, `n` / `p` move between rows, `q`
  closes.  Running it makes occur the `next-error` source, so `M-g n` /
  `M-g p` walk the matches themselves — a line with several matches is
  one row and several stops.  A search that finds nothing says so and
  leaves the previous listing alone
- `M-g` is a prefix map: `M-g g` / `M-g M-g` go to a line, `M-g n` /
  `M-g M-n` and `M-g p` / `M-g M-p` step through the results of whichever
  of compilation and occur ran most recently (Emacs'
  `next-error-last-buffer` rule)
- `M-.` (`xref-find-definitions`) asks a language server where the symbol
  at point is defined and goes there, asynchronously; `M-?`
  (`xref-find-references`) asks where it is used and lists the answer in a
  read-only `*xref*` buffer (`RET` visits, `n`/`p` move, `q` closes), and
  `M-,` (`xref-go-back`) returns to where the newest jump started; `M-TAB`
  (`completion-at-point`) completes the symbol before point and `M-x
  lsp-rename` renames it across the workspace — see
  [LSP](#lsp-optional-on-by-default) below
- File-local and directory-local variables (limited, non-evaluating
  `-*- ... -*-` modeline, `Local Variables:` footer, and a safe
  `.dir-locals.el` subset) for `compile-command` and `buffer-read-only`
- `read-only-mode` (`C-x C-q`) with buffer-local state and an `RO`
  mode-line indicator; a buffer visiting a file you cannot write comes up
  read-only by itself, as in Emacs, so the refusal arrives at the first
  keystroke rather than at the save
- No dependencies (not even curses)
- Uses standard VT100 escape sequences
- Tab stops every 8 columns by default, configurable from Lisp with
  `tab-width` (`setq` changes the default; `setq-local` changes one buffer)
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
| `-V`   | Print version (and enabled features) and exit       |
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
sudo make install          # /usr/local/{bin,share/man/man1,share/kg/lisp}
```

The nested `fe/tiny-regex-c` submodule is needed for editor regexp search.

### Windows

Windows builds use `clang-cl` with the Visual Studio 2022 C++ build tools and
Windows SDK.  Install the Visual Studio workload first, then run from a
PowerShell checkout (the script can install LLVM with `-InstallLLVM`):

```powershell
git submodule update --init --recursive
.\build-windows.ps1 -InstallLLVM
.\build\windows\Release\kg.exe -V
```

Use `-Configuration Debug` for a debug build or `-WithoutLisp` for the
smaller non-Lisp build.  The Windows port uses the native console and process
APIs; shell and compilation commands are run by `cmd.exe`, so Unix shell
commands from the examples above are not portable unchanged.

Override the prefix or use DESTDIR for staged installs:

```bash
make install prefix=/usr
make install DESTDIR=/tmp/pkg
```

To uninstall:

```bash
sudo make uninstall
```

### FreeBSD

Build with GNU make and clang; `make(1)` there is bmake and cannot read this
Makefile, and the Makefile's `CC` default is `gcc`, which FreeBSD does not
ship:

```bash
pkg install gmake                     # to build
pkg install py312-pexpect py312-pyyaml tmux   # to run `gmake check`
gmake PYTHON=python3.12 CC=clang
gmake PYTHON=python3.12 CC=clang check
sudo gmake CC=clang install
```

## Lisp

Lisp support is compiled in by default. Use `make WITH_LISP=0` to build without
the Fe Lisp interpreter;
See [LISP.md](LISP.md).

kg reads `$XDG_CONFIG_HOME/kg/init.el` (falling back to
`~/.config/kg/init.el`) at startup. One line is already a useful one — this
turns off the startup screen an empty buffer shows:

```elisp
(setq inhibit-startup-message t)
```

kg installs a handful of Lisp packages beside itself; `(require 'NAME)`
loads one. `help-fns` is the one to try first — it makes kg describe
itself:

```elisp
(require 'help-fns)
```

Then `M-x describe-function` reports what a command or function is, how it
is called interactively and what its documentation says; `M-x
describe-variable` reports a variable's value, whether that value is this
buffer's own or the global one, and whether `let` binds it dynamically;
and `M-x apropos` lists every name containing a substring. They work on
your own definitions as well as on kg's — a `defun` with a docstring and
an `(interactive ...)` declaration describes itself exactly as a built-in
does.

## Tree-sitter (optional, off by default)

Syntax highlighting has two interchangeable backends, chosen at build time.
`WITH_TREE_SITTER=0` is the default and needs nothing beyond a C compiler: it
builds the bespoke row scanners kg has always used, and it stays the
supported configuration on a machine with no package manager.

`make WITH_TREE_SITTER=1` builds against an existing tree-sitter install
instead of vendoring one. See [TREE-SITTER.md](TREE-SITTER.md).

## LSP (optional, on by default)

kg can ask a Language Server Protocol server where a symbol is defined,
where it is used and what it is, and it listens to what a server says
about a file without being asked. Unlike tree-sitter, this needs nothing at build time —
servers are found at run time — so `WITH_LSP=1` is the default and `make
WITH_LSP=0` builds the editor without it. `kg -V` says which one a binary
is:

```bash
./src/kg -V          # kg 1.1.0 +lisp -tree-sitter +lsp
```

See [LSP.md](LSP.md) for more details.


## Debugger (optional, on by default)

kg speaks the Debug Adapter Protocol, so a program can be run under a debug
adapter from inside the editor. Optional at build time (`make WITH_DAP=0`),
on by default, and `kg -V` prints `+dap` or `-dap` to say which this binary
is. See [DAP.md](DAP.md) for details.


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
make fuzz-smoke
make complexity-check
make pmccabe-check
make coverage
make compile-db
make iwyu
```

`make iwyu` runs Include What You Use from the compilation database, so
refresh `compile_commands.json` with `make compile-db` first.

`make fuzz-smoke` runs a five-second sanitizer-backed smoke campaign for
every native fuzz target.  Build one target with `make fuzz-<name>`, smoke
test it with `make fuzz-<name>-smoke`, or run it longer with its tracked
corpus.  The targets are `keypress`, `syntax`, `dirlocals`, `regex`,
`localvars`, `compile-parse`, `lsp-json`, `width`, and `keybind`; for
example:

```bash
make fuzz-syntax
FUZZ_MAX_TOTAL_TIME=60 make fuzz-syntax-smoke
./test/fuzz_syntax -artifact_prefix=test/fuzz-artifacts/syntax/ \
	test/fuzz-corpus/syntax
```

For crash triage and fuzzing notes, see [doc/FUZZING.md](doc/FUZZING.md).

## Packaging

### Debian

How to build and extract the `.deb`:

```console
# Using Podman
podman build --target export --output . -f Containerfile.deb .

# Using Docker (with BuildKit)
DOCKER_BUILDKIT=1 docker build --target export --output . -f Containerfile.deb .
```

### Alpine

How to build and extract the `.apk`:

```console
# Using Podman
podman build --target export --output . -f Containerfile.apk .

# Using Docker (with BuildKit)
DOCKER_BUILDKIT=1 docker build --target export --output . -f Containerfile.apk .
```


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
