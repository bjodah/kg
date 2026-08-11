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
  which is also how kg's own `*special*` buffers are killed
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

## Tree-sitter (optional, off by default)

Syntax highlighting has two interchangeable backends, chosen at build time.
`WITH_TREE_SITTER=0` is the default and needs nothing beyond a C compiler: it
builds the bespoke row scanners kg has always used, and it stays the
supported configuration on a machine with no package manager.

`make WITH_TREE_SITTER=1` builds against an existing tree-sitter install
instead of vendoring one. `TREE_SITTER_PREFIX` names it, and the build looks
for `$(TREE_SITTER_PREFIX)/include/tree_sitter/api.h`; the resulting binary
links `libtree-sitter` from that prefix's `lib/`, with an `-rpath` so it
still finds a shared one at run time.

```bash
make WITH_TREE_SITTER=1 TREE_SITTER_PREFIX=/usr/local
./src/kg -V          # kg 1.1.0 +lisp +tree-sitter +lsp
```

`kg -V` names every optional feature as `+word` or `-word`, so it is the way
to ask a binary which backend it has. The two flags are independent: all four
combinations of `WITH_LISP` and `WITH_TREE_SITTER` build.

Grammars are **not** linked. They are loaded at run time by soname —
`libtree-sitter-<name>.so`, the same convention Emacs' `--with-tree-sitter`
installs use, so the same grammar builds serve both. A grammar is looked for
in each colon-separated entry of `KG_TS_GRAMMAR_PATH`, and then in the
compiled-in default (`TS_GRAMMAR_PATH`, a make variable). An entry containing
`%s` has the grammar name substituted, which is how a one-directory-per-grammar
layout is a single entry:

```bash
KG_TS_GRAMMAR_PATH=/usr/lib/tree-sitter:/opt/ts-grammar-%s/lib kg foo.c
```

A grammar that is not installed is not an error and never falls back to the
row scanners: that mode is plain text for the session, said once in the status
line. The same goes for a grammar whose ABI this `libtree-sitter` cannot read.

Highlighted today: **C**, **Python**, **YAML**, **Markdown**,
**JavaScript**, **React/JSX**, **TypeScript**, **TSX**, **Java**, **Rust**,
**Go**, **HTML**, **Emacs Lisp** and **Makefile** — comments, strings, numbers,
keywords and types, from small kg-owned queries compiled into the binary,
one per language. Every other mode is plain text under
`WITH_TREE_SITTER=1`: a mode with no grammar is not an error, it simply has
no colours, and it never falls back to the row scanners.

Three details are worth knowing. Markdown uses the **block** grammar only,
so headings, fences, quotes and list markers are coloured and inline markup
inside a paragraph is not. There is no language injection, so the
JavaScript inside an HTML `<script>` and the shell inside a Makefile recipe
are plain, though the surrounding tags and `$(...)` references are not.
kg's TypeScript mode picks its grammar from the file name — `.tsx` gets the
tsx grammar, everything else typescript — because the two are different
grammars and each mis-parses the other's files. **Shell** has no grammar
yet: tree-sitter-bash exists, but the build kg is tested against ships a
release too old for this `libtree-sitter` to load.

An edit reparses incrementally, against the tree the last one left, and
re-colours only the rows that changed, so an ordinary keystroke costs what
it changed rather than what the file weighs. `WITH_TREE_SITTER=0` remains
the default, and the configuration with colours for every other language.

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

Four keys use it: **`M-.`** (`xref-find-definitions`), which goes to
the definition of the symbol at point, **`M-?`** (`xref-find-references`),
which lists every use of it, **`M-,`** (`xref-go-back`), which returns
to where the newest of those jumps started, and **`M-TAB`**
(`completion-at-point`), which completes the symbol before point. Three
more commands have no key and are typed at `M-x`: **`lsp-diagnostics`**,
which lists what the servers have reported, **`lsp-hover`**, which asks
what the symbol at point is, and **`lsp-rename`**, which renames it
everywhere.

`M-?` always lists, even for one result, in a read-only `*xref*` buffer: a
header counting the results, then one `path:line:column: preview` line
each, with paths shown relative to the workspace root. The preview is the
text of the line the result is on, with its indentation dropped and its
length capped; it comes from the buffer when the file is open, so an
unsaved edit is what the listing shows, and from a bounded read of the file
otherwise — no file is opened into a buffer to paint a listing, and a file
that has gone away simply has no preview. `RET` goes to the result on the
current line, `n` and `p` move between them, and `q` closes the listing.
The listing is bounded at 200 results and says how many more there were.
`M-.` uses the same buffer when a server offers more than one definition,
and still jumps straight to a single one.

**`M-x lsp-rename`** renames the symbol at point everywhere the server
says it appears. It prompts for the new name — the prompt spells the old
one, and an empty answer means it, which is `eglot-rename`'s default
without a minibuffer default — and applies the `WorkspaceEdit` that comes
back, in either of the two shapes a server may send it in (`changes` or
`documentChanges`). Every occurrence in one buffer is applied as a single
replacement of the span they lie in, so the whole rename is **one undo
record**: one `C-_` takes all of it back. A file no buffer visits is
opened, edited and left modified and unsaved — Emacs' behaviour, and what
lets you read the change before writing it — and the buffer you ran the
command in is the one selected again afterwards. The report is `Renamed N
occurrences in M files`.

A rename is **all or nothing across every file it names**. The whole
answer is resolved and checked first — every file opens, every buffer is
writable, every range is inside the file it names, no two edits overlap —
and only then is anything written; a single failure refuses the rename
entirely, names the file and the reason, and leaves every buffer as it
was, including any it opened only to look at. A rename that renamed most
of the uses of a symbol is worse than one that renamed none and said so.

The same applies to an answer that is no longer about your buffer. kg
sends a document to the server only when a command needs it, so between
the question and the answer the text can move; if it has — one character
typed while the server was thinking is enough — the rename is refused with
`<file> changed while the server was answering`. A server that sends
versioned `documentChanges` is answered the same way when its version is
not the one kg last sent, which is the check `eglot` makes.

Two other things a rename deliberately does not do. It does not perform
the create/rename/delete-file operations a server may attach to its
answer: they are counted and the report says how many were skipped and of
what kind. And it applies nothing at all when part of the answer cannot be
read — an edit naming something other than a local file, or more of them
than kg stores.

**`M-TAB`** (`completion-at-point`) completes the symbol before point.
It is spelled `M-TAB` because that is what a terminal sends: `C-M-i` and
`M-TAB` are the same two bytes (`ESC`, `TAB`) there, as they are in
Emacs. What is being completed is the range the server itself named when
it sent one, and otherwise the word before point — the same word `M-/`
expands. Candidates are filtered against what has been typed (a server
may return its whole set) and ordered by `sortText`, then, exactly as in
Emacs: one candidate is inserted; several sharing more than has been
typed insert what they share and say `Complete, but not unique`; several
with nothing left in common are listed in a read-only `*Completions*`
buffer beside the source, with point left where it was. An empty answer
says `No completions`. Each insertion is one edit, so `C-_` takes a
completion back in one step. A completion whose buffer changed while the
server was answering — you kept typing, as one does — is refused rather
than inserted against text that is no longer there.

The `*Completions*` listing is passive, which is a divergence worth
knowing: unlike Emacs' it binds no keys, and it is neither taken down nor
refreshed as you keep typing. It is an ordinary read-only buffer — `C-x
0` closes its window, and the next completion rebuilds it in place.

Servers are **started lazily**, and never by opening a file: the first
`M-.` or `M-?` in a C buffer is what spawns `clangd`, and every buffer under the same
workspace root then shares it. Five modes have a built-in server — `clangd`
for C, `ty server` for Python, `gopls` for Go, `rust-analyzer` for Rust and
`jdtls` for Java;
any other mode says it has no server rather than starting one. The
workspace root is the nearest ancestor holding that language's build-system
marker (`.clangd`, `compile_commands.json`, `compile_flags.txt`,
`build/compile_commands.json`; `ty.toml`, or a `pyproject.toml` mentioning
`[tool.ty`; `go.mod` or `go.work`; `Cargo.toml`; `pom.xml`, `build.gradle`,
`build.gradle.kts`, `settings.gradle` or `settings.gradle.kts`), then the
nearest ancestor holding `.git`, and failing that the file's own directory.

The nearest marker wins, which for Go and Rust is the answer their own
tooling gives: a file inside a Go module roots on that module even when a
`go.work` sits above it, because the go command finds the workspace above
the module by itself; a file in a Cargo workspace member roots on the
member, because `cargo metadata` resolves the workspace above it. Java's
markers are the ones jdt.ls itself imports a project from, and there the
rule is a compromise in one place: a Gradle subproject's `build.gradle` is
nearer than the `settings.gradle` at the top of the build, so kg starts a
server per subproject and a definition in a sibling subproject is not in
that server's model.

Both commands send the question and return. The editor stays responsive
while the server thinks, and the answer arrives later; the echo area
reports where it went, or `No definition found` / `No references found`, or
why there was no server to ask. An answer that arrives while a minibuffer
prompt is open is built but not switched to, so a half-typed filename is
never yanked out from under you — that holds for a single definition too,
which lists rather than jumps in that case. The place a jump left is
pushed on the mark ring, so `C-u C-SPC` comes back, and onto the go-back
stack, so `M-,` retraces the whole route: out of the visited file to the
`*xref*` listing, and out of that to where the search was started. Sixteen
departure points are kept as markers, so they follow the text through
later edits; one whose buffer has been killed is passed over rather than
reported, and an exhausted stack says `No xref history`.

Positions are exchanged in whichever encoding the handshake settles on.
kg offers UTF-8 first — its own columns are byte offsets, so nothing has to
be converted — and both `clangd` and `ty` take it; a server that insists on
the protocol's default, UTF-16, gets UTF-16, converted against the real
bytes of the line.

The buffer is sent to the server before each request rather than on every
keystroke, so an unsaved buffer is still the text the answer is about.
Killing a buffer tells every server holding it that the document is
closed, and quitting kg shuts every running server down.

Every request has a **deadline**, because a server that is alive but stuck
answers nothing and only death is otherwise noticed. Thirty seconds after
it was sent, an unanswered request is abandoned: the echo area says `no
reply to textDocument/definition after 30s`, the same line goes to the log
below, and that is the end of it — the server is left running, a reply that
turns up later is dropped, and the next command asks it again as usual.
`KG_LSP_TIMEOUT_MS` overrides the thirty seconds (`0` waits forever, which
is for debugging a server by hand). The default is deliberately longer than
Emacs' eglot's ten seconds: a cold clangd indexing a large project can take
that long to answer honestly, and cancelling it would turn a slow answer
into a wrong one.

`*lsp-log*` is where a server's own words go. Anything it writes to its
standard error — the complaint about a missing `compile_commands.json`, the
flags it guessed — is captured there a line at a time, prefixed with the
server's name, together with every request that ran out of time and the
reason a server died. The buffer is created on the first line and never
selects itself: `C-x b *lsp-log*` is how you read it, on the day something
hangs. It keeps the last 64 KiB, dropping whole lines from the top.

**Diagnostics** are the one part of the protocol kg never asks for: a
server publishes them whenever it has an opinion about a document it has
been told about, and `M-x lsp-diagnostics` lists what has arrived. A
publish replaces everything that server had said about that file rather
than adding to it — the protocol's own rule — and one carrying a version
older than the last one seen for that file is dropped whole, since it
describes text the server has since been sent a newer copy of.

The listing is a read-only `*Diagnostics*` buffer, shown in another window
and not selected, with one row per diagnostic as
`file:line:column: severity: message`, ordered by file and then by
position. `RET` visits the one on the current line, `n` and `p` move, `q`
closes, and showing the listing takes the `M-g M-n` / `M-g M-p` keys, so
next-error walks the diagnostics until another command produces results.
The command sends the current buffer to its server first — which is not
what Emacs' flymake does, and is what makes diagnostics reachable at all
here: kg opens a document lazily, so a session in which no LSP command had
been run would have nothing to list forever. The first
`M-x lsp-diagnostics` in a buffer therefore starts the server and asks,
and the answer arrives afterwards like every other one.

Every diagnostic is also marked in any buffer visiting its file, over the
range the server named, repainted on every publish and taken back when a
publish empties. Severity picks the mark's priority, so an error covers a
warning where the two overlap — but not its colour: the renderer's colour
channel is one foreground number and the warning face is already the red an
error wants. The severity is spelled out in the listing, which is where it
is legible.

**`M-x lsp-hover`** asks the server what the symbol at point is. There is
no key binding: kg has no eldoc, so nothing asks by itself, and Emacs has
no binding either. The answer is read in all three shapes the protocol has
had (a MarkupContent object, a bare string, a MarkedString or an array of
them) and rendered to plain text — Markdown is neutralised rather than
displayed, so fences, backticks, heading markers and horizontal rules go
and the words are what is left. The first line goes to the echo area; an
answer with more lines in it also goes whole to `*lsp-hover*`, which the
message names and `C-x b` reaches, and which never selects itself. Emacs'
eglot shows the same first line and offers the rest through `M-x
eldoc-doc-buffer` on demand; kg writes the rest unconditionally and shows
it never.

`KG_LSP_SERVER_C`, `KG_LSP_SERVER_PYTHON`, `KG_LSP_SERVER_GO`,
`KG_LSP_SERVER_RUST` and `KG_LSP_SERVER_JAVA` replace the built-in command
line for that mode. The value is run through `/bin/sh -c`, exactly as `M-x
compile`'s command is, so a wrapper, a path with spaces or extra arguments
all work — and it is how kg's own tests point the client at a fake server:

```bash
KG_LSP_SERVER_C='clangd --header-insertion=never' kg foo.c
KG_LSP_TIMEOUT_MS=5000 kg foo.c      # give up on a request after 5 s
```

One value means more than a command line. A `KG_LSP_SERVER_*` beginning
with the token `listen-hash:` and then a space runs the *rest* of the value
as the command, and speaks to it over a TCP socket instead of over its
standard input and output:

```bash
KG_LSP_SERVER_JAVA='listen-hash: nbcode --start-java-language-server=listen-hash:0' kg X.java
```

That is Oracle's nbcode, the NetBeans Java server, which does not speak LSP
on stdio at all: started that way it listens on a port, prints
`Java Language Server listening at port N with hash H` on its standard
output, and expects the client to connect to `127.0.0.1:N` and write the
hash before the first LSP byte. kg reads its stdout for that line, connects
without ever blocking the editor, sends the hash, and the frames go over the
socket both ways; everything else the server prints there, announce line
included, becomes `*lsp-log*` lines beside its standard error. Nothing
built in selects that wire — `jdtls` is still the Java default — so it
arrives with the command line that asks for it or not at all.

### Java setup

Java's server is the Eclipse JDT Language Server, and kg spawns it as the
bare name `jdtls`. It ships as a tarball rather than a package, so
`utils/install-jdtls.sh` is here to do the tedious part:

```bash
utils/install-jdtls.sh                        # newest milestone -> ~/.local
utils/install-jdtls.sh --prefix /opt/jdtls    # somewhere else
utils/install-jdtls.sh --from-source ~/src/eclipse.jdt.ls   # build a checkout
```

It needs Java 21 or newer (jdt.ls refuses to start below that) and
`python3`, whose only job is to run jdt.ls's own launcher script. The
server lands in `<prefix>/share/jdtls` and a `<prefix>/bin/jdtls` wrapper
beside it; re-running replaces the tree only once the new one is unpacked,
and the script says so and stops if `<prefix>/bin` is not on your `PATH`.
`--dry-run` prints what it would fetch and where it would put it. What it
does not do is edit your shell profile, or promise that jdt.ls will import
your project — that is jdt.ls's business, and it reports what it made of
the tree in its own log.

jdt.ls is a JVM, so the first `M-.` in a Java buffer is slower than the
first one in a C buffer: roughly two to three seconds on a warm box before
the answer arrives, and longer the first time a Maven or Gradle project is
imported. Nothing blocks while it thinks.

### Java with nbcode instead

Oracle's nbcode is the other real Java server: the NetBeans-based one
behind the Java extension for VS Code (`oracle/javavscode`). Unlike every
other server kg speaks to, it does not use stdio. Started with
`--start-java-language-server=listen-hash:0` it prints a port and a hash on
its stdout and then waits to be *connected to*, and the client has to open
a TCP socket to that port and write the hash before the first LSP byte.
The `listen-hash:` token in front of a `KG_LSP_SERVER_<MODE>` command line
is what asks kg for that wire; everything after it is the command, run
through `/bin/sh -c` as usual:

```bash
utils/install-nbcode.sh    # runtime -> ~/.local/share, wrapper -> ~/.local/bin
export KG_LSP_SERVER_JAVA="listen-hash: nbcode --start-java-language-server=listen-hash:0"
```

`install-nbcode.sh` takes the same options as the jdtls one — `--prefix`,
`--version`, `--from-source`, `--dry-run` — and by default gets nbcode the
cheap way, because the published extension is a zip with a complete
NetBeans runtime inside it: it downloads that from open-vsx.org (~150 MB)
and unpacks `extension/nbcode` into `<prefix>/share/nbcode`.
`--from-source` builds a javavscode checkout with Ant instead, which wants
that checkout's `netbeans` submodule fetched and `ant apply-patches`
already run, and takes tens of minutes. Java 17 or newer either way.

The `<prefix>/bin/nbcode` wrapper exists to give each run a userdir of its
own, and that is not fussiness: NetBeans' single-instance handler is keyed
on the userdir, so two nbcode processes pointed at one do not both start —
the second hands its command line to the first and exits without ever
printing a port, which looks from the outside like a server that started
and said nothing. Set `KG_NBCODE_USERDIR` to pin one anyway and keep its
index warm between sessions, at the price of running one nbcode at a time.

Being a NetBeans, nbcode is in the same class as jdt.ls rather than faster:
measured here on a one-file Maven project, about a second and a half from
spawn to the announce line and around three seconds to the first `M-.`
answer, cold. A large project's first import is longer, and with a
per-run userdir it is paid every session.

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
