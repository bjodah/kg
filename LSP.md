## LSP (Language Server Protocol) in kg

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

The `*Completions*` listing **closes itself once point leaves the symbol
it is a listing of** — Emacs' rule, and Emacs' surprising half of it too:
typing more of that symbol leaves the listing up (Emacs does not refresh
it either, and neither does kg), while a space, a `C-a` back past where
the completion began, or a move to another line takes it down. Stepping
into it with `C-x o` keeps it, so it does not vanish as you arrive.
Repeating `M-TAB` keeps it and replaces it when the next answer lands.
The listing is still passive in the one way it was: unlike Emacs' it
binds no keys. It is an ordinary read-only buffer — `C-x 0` closes its
window by hand, and closing it does not kill it.

Servers are **started lazily**, and never by opening a file: the first
`M-.` or `M-?` in a C buffer is what spawns `clangd`, and every buffer under the same
workspace root then shares it. Five modes have a built-in server — `clangd`
for C, `ty server` for Python, `gopls` for Go, `rust-analyzer` for Rust and
`nbcode` for Java;
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
markers are the ones a Java server itself imports a project from, and there the
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
included, becomes `*lsp-log*` lines beside its standard error. Java's own
built-in row selects that wire, since nbcode is the Java default; the token
is what an *override* needs when it points at another nbcode.

An override otherwise speaks stdio, whatever the row it replaces uses. So
`KG_LSP_SERVER_JAVA=jdtls` is jdt.ls on its own arrangement even though
Java's built-in row is a socket — the wire belongs to the command, and an
override that inherited nbcode's would write a handshake at a server with
nothing to answer it with.

### Java setup

Java's server is Oracle's nbcode — the NetBeans-based one behind the Java
extension for VS Code (`oracle/javavscode`) — and kg spawns it as the bare
name `nbcode`. It is the default because it is also kg's *debugger* for
Java: one nbcode process announces a language server and a Java debug
adapter on one stdout, so debugging Java means talking to the server that
is already open rather than starting a second toolchain.

Unlike every other server kg speaks to, nbcode does not use stdio. kg
starts it with `--start-java-language-server=listen-hash:0` and
`--start-java-debug-adapter-server=listen-hash:0`; it prints a port and a
128-character hash per server on its stdout and then waits to be *connected
to*, and kg opens a TCP socket to that port and writes the hash before the
first LSP byte. Nothing blocks while that happens.

```bash
utils/install-nbcode.sh    # runtime -> ~/.local/share, wrapper -> ~/.local/bin
```

`install-nbcode.sh` takes the same options as the jdtls one below —
`--prefix`, `--version`, `--from-source`, `--dry-run` — and by default gets
nbcode the cheap way, because the published extension is a zip with a
complete NetBeans runtime inside it: it downloads that from open-vsx.org
(~150 MB) and unpacks `extension/nbcode` into `<prefix>/share/nbcode`.
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

Being a NetBeans, nbcode is a JVM rather than something fast: measured
here on a one-file project, about a second and a half from spawn to the
announce lines and around three seconds to the first `M-.` answer, cold. A
large project's first import is longer, and with a per-run userdir it is
paid every session.

kg sends nbcode one thing it sends no other server: an
`initializationOptions.nbcodeCapabilities` object, which is what turns its
Java support on and names the command namespace it answers under. Every
flag in it for a facility kg does not have — a status bar, a test-results
view, an HTML page renderer — is sent as false, because advertising a
facility kg lacks is how a client ends up being sent messages it silently
drops.

### Java with jdt.ls instead

The Eclipse JDT Language Server was kg's Java default before nbcode and is
still one override away:

```bash
export KG_LSP_SERVER_JAVA=jdtls
```

kg cannot debug Java through it. The Java debug adapter is a socket the
nbcode process announces beside its language server, so a `dap-debug` in a
Java buffer whose server is jdt.ls says so rather than starting something.

jdt.ls ships as a tarball rather than a package, so
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
