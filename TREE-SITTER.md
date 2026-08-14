## Tree-sitter in kg

 `TREE_SITTER_PREFIX` names it, and the build looks
for `$(TREE_SITTER_PREFIX)/include/tree_sitter/api.h`; the resulting binary
links `libtree-sitter` from that prefix's `lib/`, with an `-rpath` so it
still finds a shared one at run time.

```bash
make WITH_TREE_SITTER=1 TREE_SITTER_PREFIX=/usr/local
./src/kg -V          # kg 1.1.0 +lisp +tree-sitter +lsp
```

On a machine with no such install, `utils/build-tree-sitter.sh` builds one:
it clones the core and the grammars kg's registry asks for at the revisions
`utils/tree-sitter-pins` names, compiles each grammar from its checked-in
`src/parser.c` (no tree-sitter CLI, no cargo, no npm), and prints the path of
the prefix it cached under `$KG_TS_CACHE` (default `~/.cache/kg-tree-sitter`).
It wants a C compiler, `git` and network access, and takes about twenty
seconds. This is what `.ci/ci-13-with-tree-sitter.sh` falls back to when the
box has no prefix of its own.

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
**Go**, **HTML**, **Emacs Lisp**, **Makefile** and **Shell** — comments,
strings, numbers,
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
grammars and each mis-parses the other's files.

An edit reparses incrementally, against the tree the last one left, and
re-colours only the rows that changed, so an ordinary keystroke costs what
it changed rather than what the file weighs. `WITH_TREE_SITTER=0` remains
the default, and the configuration with colours for every other language.
