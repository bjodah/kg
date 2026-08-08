# Tree-sitter Integration Plan

## Prompt
In bjodah/kg on branch "stricter-emacs-adherence", we currently can compile WITH_LISP=0 or 1, I'd like a new optional dependency (
github.com/tree-sitter/tree-sitter): `WITH_TREE_SITTER=1`: I think we need to factor out current bespoke syntax highlighting
logic etc. so that it is only compiled when WITH_TREE_SITTER=0, and when WITH_TREE_SITTER=1, we leverage tree-sitter as much as possible. Write a detailed implementation plan.

## Response
I inspected bjodah/kg on stricter-emacs-adherence, and I think your instinct is right: the bespoke highlighter should become one compile-time backend, not remain intertwined with the editor's notion of “syntax/mode.” I would avoid simply wrapping most of syntax.c in `#if !KG_USE_TREE_SITTER`, because several things currently living there are not actually legacy highlighting logic.
At the moment syntax.h/syntax.c combine at least four responsibilities: the HL_* face representation used by display, filename/shebang → mode selection, the handcrafted scanners/keyword databases, and semantic identity for special modes such as Git commit/rebase.  Display, however, is already nicely decoupled: it consumes row->hl[], lets decorations override those values, and maps HL_* to terminal colors. That is the seam I would preserve.
There is also a particularly good integration point for incremental Tree-sitter: edits are now substantially centralized in kg_buffer_replace(), with flat old/new byte ranges known before publication.  That maps almost exactly onto Tree-sitter's TSInputEdit, whose contract is start_byte, old_end_byte, new_end_byte plus old/new row/column points, followed by ts_tree_edit() and a parse using the old tree.
Important dependency point
tree-sitter/tree-sitter by itself is not sufficient to replace the existing highlighter. The core provides the parser engine; each language needs a generated TSLanguage, normally coming from a separate grammar repository, and syntax highlighting normally also relies on that grammar's highlight queries.
So I would define WITH_TREE_SITTER=1 as meaning:

Build the Tree-sitter highlighting backend, compile the Tree-sitter core plus the set of grammar parsers that kg supports, and do not compile the legacy syntax scanner/database.

The core is easy to embed in this C project: Tree-sitter explicitly supports compiling tree-sitter/lib/src/lib.c directly with lib/src and lib/include on the include path. No Rust/Node runtime is needed for that. As of August 7, 2026, the current Tree-sitter release is v0.26.11; I would pin that exact release/commit initially rather than tracking its default branch.

Proposed target architecture
I would aim for approximately this source layout:

```
src/
    syntax.h                    public, backend-neutral API
    syntax.c                    common facade / registry / colors / mode selection
    syntax_backend.h            private backend contract

    syntax_legacy.c             current bespoke scanners and language DB
    syntax_tree_sitter.c        parser/tree/edit/query machinery
    syntax_tree_sitter_lang.c   TS language/query registry

    gitcommit.c                 Git commit mode semantics
    gitrebase.c                 Git rebase mode semantics

mk/
    tree-sitter.mk              third-party core/grammar build rules

utils/
    embed_tree_sitter_queries.py

tree-sitter/                    submodule: tree-sitter/tree-sitter
tree-sitter-grammars/
    c/
    python/
    bash/
    ...
```

The important property is:

```
WITH_TREE_SITTER=0:
    syntax.c
    syntax_legacy.c
    no Tree-sitter objects

WITH_TREE_SITTER=1:
    syntax.c
    syntax_tree_sitter.c
    syntax_tree_sitter_lang.c
    tree-sitter core + grammar objects
    NO syntax_legacy.o
```

I would prefer source-list selection in the Makefile over large preprocessor branches inside syntax_legacy.c. That makes “legacy code is not compiled” mechanically true.

### Phase 1 — Separate mode identity from highlighting
This should be the first, completely behavior-preserving change.
Currently struct editor_syntax contains filename patterns, keywords, comment delimiters, flags, and the function pointer for a custom highlighter. More problematically, mode semantics inspect that highlighter pointer: syntax_is_git_commit() and syntax_is_git_rebase() identify their mode via equality with gitcommit_syntax/gitrebase_syntax. The keyboard layer then uses those functions to activate the special Git keymaps.
And bufmgr.c creates synthetic struct editor_syntax instances for IBuffer, Text, Lisp Interaction, and Compilation—even though those are clearly editor modes, not syntax-highlighting algorithms.
I would add stable mode identity first:

```C
enum kg_mode_id {
    KG_MODE_TEXT,
    KG_MODE_C,
    KG_MODE_PYTHON,
    KG_MODE_SHELL,
    ...
    KG_MODE_GIT_COMMIT,
    KG_MODE_GIT_REBASE,
    KG_MODE_IBUFFER,
    KG_MODE_COMPILATION,
    KG_MODE_LISP_INTERACTION,
};
```

Then make the common record roughly:

```C
struct editor_syntax {
    enum kg_mode_id id;
    const char *name;
    const char *const *filematch;
};
```

I would probably retain the name editor_syntax during this migration to avoid a giant rename diff. Architecturally it is becoming a mode descriptor; renaming it to editor_mode can be a later cleanup.
Then:

```C
int syntax_is_git_commit(void)
{
    return bcur()->syntax &&
           bcur()->syntax->id == KG_MODE_GIT_COMMIT;
}
```

and likewise for rebase.
Acceptance criterion: every existing mode-selection, Git keymap, Lisp mode, buffer-list, compilation, and PTY test behaves identically, but no non-highlighting behavior relies on a highlighter function pointer anymore.

### Phase 2 — Split common syntax code from syntax_legacy.c
Next move all genuinely bespoke material out of syntax.c.
syntax_legacy.c should receive:

the large keyword arrays;

comment delimiters and legacy flags;

generic_keyword_scan;

Markdown scanner;

Makefile scanner;

YAML scanner;

Git commit/rebase highlighting routines;

hl_oc propagation implementation;

is_separator;

editor_row_has_open_comment;

other functions used solely to implement those scanners.

syntax.c should retain:

HL_* → color mapping;

common highlight-buffer allocation helpers;

filename/basename/shebang selection;

mode registry and syntax_find_by_name;

editor_set_syntax() and KG_EVENT_MODE_CHANGED;

backend dispatch;

special mode identity;

semantic helpers that aren't actually highlighting.

In particular, these should stay available in both configurations:

```C
syntax_git_rebase_pick_span()
syntax_git_rebase_flags_end()
syntax_git_commit_subject()
```

The first two are already command semantics, not fontification—the rebase editing commands need to understand those lines independently of whether a highlighter is installed.
I'd also stop having test/test_syntax.c declare extern struct editor_syntax HLDB[]; the mode registry should have a proper public lookup API instead. The existing test suite reaches fairly deeply into the current implementation.
Private legacy metadata
Rather than keeping legacy fields in the common mode record, introduce something like:

```C
struct legacy_syntax_spec {
    enum kg_mode_id mode;
    const char *const *keywords;
    const char *single_comment;
    const char *multi_comment_start;
    const char *multi_comment_end;
    unsigned flags;
    void (*highlight)(struct editor_buffer *, struct erow *);
};
```

The legacy backend finds this by mode->id.
That is the key factoring step: the mode registry says what a buffer is; the selected backend says how that mode is highlighted.

### Phase 3 — Make highlight updates edit-granular
This is the most important prerequisite for a good Tree-sitter implementation.
Right now editor_update_row() does two jobs:

rebuild row->render;

immediately call editor_update_syntax(b, row).

That row-at-a-time model makes sense for the current scanner, but it is the wrong lifecycle for Tree-sitter. A multiline paste affecting ten rows should produce one incremental Tree-sitter edit and one reparse, not ten reparses while the new buffer topology is half-installed.
I would split these responsibilities.
Something along these lines:

```C
void editor_render_row(struct editor_buffer *b, erow *row);

struct kg_syntax_edit {
    size_t start_byte;
    size_t old_end_byte;
    size_t new_end_byte;

    struct kg_text_point start_point;
    struct kg_text_point old_end_point;
    struct kg_text_point new_end_point;
};

void syntax_backend_after_edit(
    struct editor_buffer *,
    const struct kg_syntax_edit *);

void syntax_backend_rebuild(struct editor_buffer *);
```

where:

```C
struct kg_text_point {
    uint32_t row;
    uint32_t column;   /* raw source bytes, not render columns */
};
```

Then kg_buffer_replace() becomes:

```
calculate old r0/c0, r1/c1
calculate kg_syntax_edit
stage edit
publish new text/rows
render every changed row
update markers/decorations
update syntax backend ONCE
publish KG_EVENT_TEXT_CHANGED
```

This preserves a nice invariant: by the time the existing text-change event is published, both the buffer's bytes and its derived syntax state represent the new content.
New-end point calculation
Tree-sitter columns are byte counts from the start of a line, which is exactly the coordinate system kg already uses for row->chars. [Image or Canvas]tree-sitter.github.io
For a replacement:

```C
new_end_byte =
    start_byte + replacement_len;
```

and:

with no '\n' in replacement: new_end.column = start.column + len;

otherwise:

new_end.row = start.row + newline_count;

new_end.column = bytes_after_last_newline.

No render/tab conversion belongs here.
Remaining raw mutation paths
I would make a deliberate audit part of this phase. kg_buffer_replace() is now the obvious primary gateway, but there are still raw row mutation helpers, and kg_buffer_append_internal() is one confirmed live path using them.
For each remaining live raw mutation, choose one of:

migrate it onto kg_buffer_replace(), preferably; or

call syntax_backend_rebuild(b) after the complete operation.

Do not try to synthesize half-correct incremental edits from scattered row mutations.
The goal should eventually be: ordinary user edits always go through the edit transaction; broad/internal replacement has an explicit “whole buffer changed” syntax hook.

### Phase 4 — Fix staged file loading for a whole-buffer parser
There is one subtle architectural issue here.
load_stage_rows() currently selects syntax into a temporary probe and runs kg_row_builder_highlight() before the rows are adopted.  That works because the bespoke highlighter proceeds row-by-row.
Tree-sitter wants the complete logical document.
I would split:

```C
kg_row_builder_highlight()
```

into conceptually:

```C
kg_row_builder_render()
syntax_backend_prepare()
```

For a transactional file load:

```
read all staged chars rows
render all staged rows
select mode
construct temporary editor_buffer over those rows
prepare backend state + highlights against whole temporary buffer
adopt rows
transfer prepared syntax state into real buffer
```

This keeps the current nice property that file loading doesn't publish a half-prepared buffer.
syntax_backend_prepare() can return an opaque:

```C
struct kg_syntax_state;
```

which is owned by the resulting buffer. editor_buffer therefore gains:

```C
struct kg_syntax_state *syntax_state;
```

The type remains opaque: do not put TSParser, TSTree, or <tree_sitter/api.h> into def.h. That keeps WITH_TREE_SITTER=0 genuinely independent of Tree-sitter headers and avoids poisoning the project's standalone-header checks.
The state must be released on:

buffer kill;

slot reuse;

editor cleanup;

mode change;

broad replacement when it isn't being replaced by a prepared state.

### Phase 5 — Add WITH_TREE_SITTER to the build system
Only after the seams above exist would I add the dependency.
The Makefile currently handles WITH_LISP cleanly enough to copy its ergonomics: validation, missing-submodule diagnostics, a build-configuration stamp, conditional source lists, and a dedicated WITH_LISP=0 CI lane.
I would add:

```
MakefileWITH_TREE_SITTER ?= 0

ifneq ($(WITH_TREE_SITTER),0)
ifneq ($(WITH_TREE_SITTER),1)
$(error WITH_TREE_SITTER must be 0 or 1)
endif
endif

ifeq ($(WITH_TREE_SITTER),1)
override CFLAGS += -DKG_USE_TREE_SITTER=1
SYNTAX_BACKEND_SRCS = syntax_tree_sitter.c syntax_tree_sitter_lang.c
include mk/tree-sitter.mk
else
SYNTAX_BACKEND_SRCS = syntax_legacy.c
endif
```

and:

```
MakefileSRCS = main.c tty.c syntax.c $(SYNTAX_BACKEND_SRCS) ...
```

Generalize the build stamp
The current:

```
MakefileLISP_CONFIG = src/.with-lisp-$(WITH_LISP)
```

protects against stale objects when switching Lisp configurations.
I would replace it with one feature stamp:

```
MakefileFEATURE_CONFIG = \
    src/.features-lisp-$(WITH_LISP)-tree-sitter-$(WITH_TREE_SITTER)
```

and make every project object depend on that.
Otherwise this sequence is dangerous:

```Bash
make WITH_TREE_SITTER=0
make WITH_TREE_SITTER=1
```

because an object compiled without KG_USE_TREE_SITTER may otherwise be considered up to date.
The CI runner's copy/cleanup exclusions also explicitly know about .with-lisp-*, so that needs updating at the same time.
Third-party compilation
I would compile Tree-sitter core separately from kg's own SRCS:

```
MakefileTS_CORE_SRC = tree-sitter/lib/src/lib.c
TS_CORE_OBJ = $(TS_OBJDIR)/lib.o
```

with:

```
-Itree-sitter/lib/include
-Itree-sitter/lib/src
```

exactly as Tree-sitter recommends. [Image or Canvas]tree-sitter.github.io
Grammar generated parser.c files likewise get separate object rules. Some grammars have an external scanner.c or scanner.cc, so mk/tree-sitter.mk should support both C and C++ scanner objects from day one rather than assuming all grammars are pure C.
Vendored/generated Tree-sitter sources should not enter kg's IWYU, complexity, formatting, or project warning-baseline lists.
Version output
I'd extend:

```
kg 1.1.0 +lisp
```

to, say:

```
kg 1.1.0 +lisp +tree-sitter
kg 1.1.0 +lisp -tree-sitter
```

The current -V implementation only reports Lisp.  This gives both users and CI an inexpensive way to verify which binary they built.

### Phase 6 — Tree-sitter buffer implementation
Per buffer, I'd hold:

```C
struct kg_syntax_state {
    TSParser *parser;
    TSTree *tree;
    const struct kg_ts_language_spec *language;
    TSQueryCursor *query_cursor;
};
```

The TSQuery itself should preferably be shared by all buffers of a language because Tree-sitter queries are immutable; query cursors hold mutable execution state and are reusable. [Image or Canvas]tree-sitter.github.io
A language spec might be:

```C
struct kg_ts_language_spec {
    enum kg_mode_id mode;
    const TSLanguage *(*language)(void);
    const char *highlight_query;
    size_t highlight_query_len;
};
```

There will need to be variants for things such as TypeScript versus TSX; don't assume one editor mode necessarily corresponds one-to-one with one TSLanguage.
Feed Tree-sitter directly from kg's rows
Do not call editor_rows_to_string() before every parse.
Tree-sitter's TSInput API exists specifically for editors whose text lives in a rope/piece-table/custom representation: its callback receives both the byte offset and row/column point. [Image or Canvas]tree-sitter.github.io
Use:

```C
TSInput {
    .payload = b,
    .read = kg_ts_read,
    .encoding = TSInputEncodingUTF8,
}
```

kg_ts_read() should expose the logical sequence:

```
row[0].chars
'\n'
row[1].chars
'\n'
...
row[n-1].chars
```

without an extra newline after the final row.
For a first implementation, returning row fragments plus a one-byte newline fragment is correct. I would probably add an 8 KiB scratch chunk shortly afterwards so a 100k-line file doesn't incur roughly two callback invocations per line during a full parse.
Also add an explicit guard for documents whose Tree-sitter byte coordinates won't fit its 32-bit position fields. A >4 GiB buffer should simply fall back to unhighlighted mode rather than truncate an offset.

### Phase 7 — Incremental parse and damaged-range highlighting
For every successful kg_buffer_replace():

```C
TSInputEdit edit = ...;

ts_tree_edit(state->tree, &edit);

TSTree *new_tree =
    ts_parser_parse(state->parser, state->tree, input);
```

This is Tree-sitter's intended editor workflow and lets the new tree reuse unchanged structure from the old tree. [Image or Canvas]tree-sitter.github.io
I would retain the existing row->hl[] representation rather than invent a Tree-sitter-specific rendering path.
For each highlight capture:

```
TS capture raw row/column
        ↓
row->chars byte offsets
        ↓
chars_to_render_col()
        ↓
row->hl[] render offsets
```

That last conversion matters because tabs expand in render while Tree-sitter talks about source bytes.
For a multiline capture, apply it row by row:

```
first row: capture.start.column → EOL
middle:    0 → EOL
last row:  0 → capture.end.column
```

then convert each pair to render offsets.
Highlight damage
I would not re-query the whole file on every keystroke; that would throw away much of the reason for using Tree-sitter.
Build a damage range from:

the new edit span itself;

Tree-sitter's changed ranges between the edited old tree and new tree;

whole source rows intersecting those ranges.

Tree-sitter's query cursor can restrict execution to byte or point ranges, and matches intersecting that range are still returned. [Image or Canvas]tree-sitter.github.io
The critical test, however, should be incremental result versus full-rebuild result. More on that below. That lets us begin conservatively and shrink the damage window only where correctness is proven.

### Phase 8 — Grammar and highlight-query policy
I would make a small manifest before coding this part, listing all 24 current registry entries:

```
C
Python
Shell
JavaScript
Rust
Java
TypeScript
C#
PHP
Ruby
Swift
SQL
Dart
HTML
React
Vue
Angular
Svelte
Makefile
Markdown
Lisp
Git commit
Git rebase
YAML
```

For every entry record:

```
kg mode
grammar repository
pinned commit
tree_sitter_*() symbol
parser.c
scanner.c / scanner.cc, if any
highlights query
license
injection requirements
ABI compatibility
```

Then classify them.
Ordinary languages can map straightforwardly onto grammar submodules.
React should probably be a JS/JSX grammar variant, not its own duplicated language dependency.
TypeScript/TSX likewise needs explicit grammar selection.
Vue/Angular/Svelte need an explicit decision about embedded-language/injection support.
Git commit/rebase should remain special kg modes regardless of whether we find a satisfactory grammar. Their command/keymap behavior must never depend on a third-party parser.
Queries are a separate implementation concern
Tree-sitter highlighting uses query captures such as @keyword, @function, @type, @string, etc. [Image or Canvas]tree-sitter.github.io Map them onto kg's intentionally small palette, for example:

```
comment.*         -> HL_COMMENT
string.*          -> HL_STRING
number.*          -> HL_NUMBER
keyword.*         -> HL_KEYWORD1
operator          -> HL_KEYWORD1
type.*            -> HL_KEYWORD2
function.*        -> HL_KEYWORD2
constructor       -> HL_KEYWORD2
constant.*        -> HL_KEYWORD2
```

I'd define a deterministic precedence table rather than letting traversal order accidentally decide overlapping captures.
There is an important trap here: do not blindly load arbitrary upstream highlights.scm files and ignore their predicates. Tree-sitter's C query layer exposes predicates/directives, but the docs explicitly say they are not executed by the core C library itself; higher-level code must implement them. [Image or Canvas]tree-sitter.github.io
For the first implementation I would therefore choose one of two explicit policies:

maintain small kg-owned highlighting queries, tailored to our limited HL_* palette and using only query features we support; my preference initially; or

consume upstream queries, but implement every predicate/directive used by the selected set and make startup/tests reject unsupported ones.

Silently treating a predicate as true is not acceptable.
Embed queries
I would embed the chosen .scm query text into the binary, analogous to how the Lisp prelude is embedded now, rather than add runtime lookup paths and install-time query directories.
Add something like:

```
utils/embed_tree_sitter_queries.py
src/tree_sitter_queries_generated.inc
make tree-sitter-queries-generate
make tree-sitter-queries-check
```

Then a normal installed kg has no runtime dependency beyond its own binary.

### Phase 9 — Special Git modes
I would explicitly separate their mode semantics from their old highlighter.
For WITH_TREE_SITTER=1:

exact COMMIT_EDITMSG/git-rebase-todo detection still works;

C-c mode-specific keymaps still work;

syntax_git_rebase_pick_span() and flag manipulation still work;

commit subject discovery still works.

If we do not initially have suitable grammars, those buffers can have normal text coloring under the Tree-sitter backend. That is much better than dragging gitcommit_syntax() and gitrebase_syntax() into the new build as hidden legacy fallbacks.
The two current diagnostic behaviors—overlong commit subjects and bad rebase actions—are arguably editor diagnostics, not syntax highlighting. I would eventually express their HL_WARNING spans as decorations, since decorations already deliberately override syntax faces in display.c. That preserves useful behavior in both backends without making Tree-sitter depend on a custom lexer.

### Phase 10 — Tests
I would split the current syntax suite into three layers.
Always built: test_syntax_common
Test:

filename selection;

shebang selection;

exact Git basename behavior;

mode IDs;

mode-change event;

color mapping;

Git semantic helpers;

render↔source offset conversion;

special keymap activation.

WITH_TREE_SITTER=0: test_syntax_legacy
Move essentially all current scanner-specific expectations here:

exact C keyword classifications;

handmade numeric recognition;

Markdown fence propagation;

Makefile scanner behavior;

YAML block-state behavior;

hl_oc propagation;

custom highlighter pointer tests.

This suite should be green before and after the refactor, proving that moving the code did not change the existing backend.
WITH_TREE_SITTER=1: test_syntax_tree_sitter
Add tests for:

every configured grammar can be assigned to a parser;

every embedded query compiles;

no unsupported query predicate/directive exists;

representative captures map to expected HL_*;

tabs before a captured token map correctly into row->render;

UTF-8 preceding a token keeps byte coordinates correct;

multiline string/comment captures;

newline insertion/deletion;

multiline paste;

undo;

mode switch;

file reload/broad replacement;

buffer kill and parser/tree cleanup;

unsupported mode → clean HL_NORMAL, no crash.

The most valuable regression test would be:

```
1. parse/highlight fixture
2. apply an edit incrementally
3. save resulting row->hl arrays
4. throw Tree-sitter state away
5. full-parse/full-highlight same final text
6. assert every row->hl byte is identical
```

Run that over a table of awkward edits and then under a fuzzer.
That test gives permission to optimize the damaged-range logic without guessing.
I'd also add a Tree-sitter variant of the existing keypress fuzz target; your current fuzz build explicitly lists src/syntax.c, so its source/object composition must become backend-aware too.

Phase 11 — CI feature matrix
WITH_LISP and WITH_TREE_SITTER should be strictly orthogonal:
WITH_LISPWITH_TREE_SITTERWhat it proves00minimal/current legacy editor10existing default behavior01Tree-sitter has no accidental Fe/Lisp dependency11full-feature build
I would keep the current dedicated WITH_LISP=0 check and add an analogous Tree-sitter lane. The current GitHub build workflow already checks out recursive submodules and tests GCC/Clang/Linux/macOS.
If the grammar set becomes sizable, I would stop checking out all submodules recursively in every default build. Instead:

```
legacy lanes:
    initialize fe only when WITH_LISP=1

Tree-sitter lanes:
    initialize tree-sitter + grammar submodules

full lane:
    initialize both
```

That reinforces that the dependency really is optional.
Add hard assertions such as:

```Bash
make clean
make WITH_TREE_SITTER=1
test ! -e src/syntax_legacy.o
./src/kg -V | grep '+tree-sitter'

make clean
make WITH_TREE_SITTER=0
test ! -e src/syntax_tree_sitter.o
./src/kg -V | grep -- '-tree-sitter'
```

I would run the full PTY acceptance suite on at least (1,0) and (1,1). The other two configurations can run unit/build tests if four copies of the entire PTY/Emacs oracle suite become too costly.

Phase 12 — Performance instrumentation
The existing syntax counters describe a row scanner—SYNTAX_ROW, syntax bytes scanned, propagation, etc.—so I would not make Tree-sitter artificially increment counters with different semantics.
Add backend-specific counters such as:

```
KG_PERF_TS_PARSE
KG_PERF_TS_FULL_PARSE
KG_PERF_TS_INPUT_BYTES
KG_PERF_TS_CHANGED_RANGE
KG_PERF_TS_HIGHLIGHT_QUERY
KG_PERF_TS_CAPTURE
KG_PERF_TS_REHIGHLIGHT_ROW
```

Then add a benchmark for:

opening a large C/Python file;

inserting one character near the top;

inserting a newline;

deleting a quote/comment delimiter;

multiline paste.

The crucial assertions are qualitative:

```
single-character edit:
    no whole-buffer serialization
    no new parser allocation
    incremental parse using old tree
    bounded highlighting damage in ordinary cases
```

Tree-sitter's custom TSInput means there is no reason for an incremental edit to construct a second contiguous copy of the buffer. [Image or Canvas]tree-sitter.github.io

Commit-sized sequence I would actually use
I would implement this as roughly these commits, each independently reviewable:

syntax: give modes stable identities
Decouple Git/special-mode behavior from highlighter callbacks.

syntax: separate common registry from legacy scanner
Create syntax_legacy.c; zero behavior change.

buffer: make syntax updates edit-granular
Separate row rendering and syntax notification; central edit/broad-change hooks.

syntax: make staged loads backend-preparable
Whole-buffer syntax-state preparation and ownership transfer.

build: add WITH_TREE_SITTER feature axis
Makefile validation, feature stamp, -V, core submodule, CI skeleton.

tree-sitter: add C backend and row-backed TSInput
One language first, probably C, proving parser/tree lifecycle and offsets.

tree-sitter: incremental edits and ranged highlighting
TSInputEdit, old-tree reuse, differential incremental/full tests.

tree-sitter: add grammar/query registry
Expand ordinary language coverage in batches.

git modes: separate diagnostics from syntax backend
Preserve warnings via common decorations if desired.

ci: cover feature matrix and backend exclusion
All four WITH_* combinations, ASAN/fuzz/perf where appropriate.

docs: document Tree-sitter builds and grammar coverage.

Definition of done
I would not call WITH_TREE_SITTER=1 complete until these are all true:

```
[ ] WITH_TREE_SITTER accepts exactly 0/1 and defaults to 0.
[ ] WITH_LISP and WITH_TREE_SITTER are independent.
[ ] syntax_legacy.c is not compiled in a TS build.
[ ] Tree-sitter headers are invisible to normal public kg headers.
[ ] no live edit reparses once per affected row.
[ ] ordinary edits use TSInputEdit + old-tree parsing.
[ ] no parse path serializes the entire buffer merely to feed Tree-sitter.
[ ] mode identity/keymaps do not depend on a highlighting backend.
[ ] staged file loads have correct Tree-sitter state before publication.
[ ] every supported grammar/query is pinned and tested.
[ ] unsupported languages degrade to normal text, never legacy fallback.
[ ] incremental highlighting is differential-tested against a full rebuild.
[ ] both backends feed the existing row->hl/display path.
[ ] all four Lisp × Tree-sitter configurations build in CI.
```

The architectural step I would put the most emphasis on is Phase 3, moving from “a row was rendered, therefore run syntax now” to “one logical buffer edit committed, therefore notify the syntax backend once.” The branch's recent kg_buffer_replace() work has already done most of the hard prerequisite for that. Once that seam exists, Tree-sitter becomes a fairly natural second implementation instead of a pile of exceptions.

---

## Refinement — 2026-08-07 (reviewed against the tree; decisions recorded)

This section is authoritative where it disagrees with the Response
above.  The Response's architecture stands (the `row->hl[]` seam, the
`kg_buffer_replace()`/`TSInputEdit` anchor, the differential
incremental-vs-full test, source-list backend selection); what follows
corrects its facts, records the project decisions, and replaces its
dependency model.

A timely motivation data point: the 2026-08-07 `launch.sh` crash was a
1-byte heap overflow in `generic_keyword_scan()`'s in-string escape
handling (`src/syntax.c:1650`, present since the kilo ancestry) — the
bespoke scanners are exactly the class of hand-rolled parsing this
plan retires from the default of new language support.

### Decisions (user, 2026-08-07)

1. **Dependency model: prebuilt prefixes, not submodules.**  The
   development environment provides `/opt-9/tree-sitter-v0.26.11-
   {release,debug,asan,msan}` (guard: `include/tree_sitter/api.h`) and
   17 grammar prefixes `/opt-9/tree-sitter-grammar-<name>/lib/
   libtree-sitter-<name>.so`, each pinned to an upstream tag (table
   below).  No `tree-sitter/` or grammar submodules in kg.  The
   grammar sonames follow **Emacs' own convention** (these installs
   were built for Emacs `--with-tree-sitter`), so kg adopts Emacs'
   loading model too: grammars are **dlopen'd at runtime** by soname,
   resolved through a search path (`KG_TS_GRAMMAR_PATH`, colon-
   separated, plus a compiled-in default), symbol `tree_sitter_<lang>`.
   A missing grammar library degrades that mode to plain text with a
   message — never an error, never a legacy fallback.  The core
   library is an ordinary link-time dependency of the TS build,
   located by `TREE_SITTER_PREFIX` (Makefile var), no pkg-config
   requirement.
2. **Query policy: kg-owned minimal queries**, tailored to the HL_*
   palette, embedded in the binary (the Response's option 1).  No
   upstream `highlights.scm` consumption, therefore no predicate
   engine.
3. **The legacy backend is permanent** (not deprecation-staged): kg
   must keep building quickly on esoteric platforms with no package
   manager, so `WITH_TREE_SITTER=0` remains the default and the
   dependency-free configuration, and `test_syntax_legacy` is a fully
   maintained first-class suite, not a museum.
4. **Language injections are out of v1.**  `markdown-inline` is not
   loaded (Markdown uses the block grammar only); Vue/Angular/Svelte/
   React/HTML-embedded-JS get no TS support initially and degrade to
   plain text under `WITH_TREE_SITTER=1`.
5. **Sequencing: after Phase 12** of the elisp/fe program closes.
   Nothing here starts while that program holds the caps and gates.

### Grammar manifest (pinned by the /opt-9 environment)

Available (tag): bash v0.6.0 *(too old: grammar ABI 6)*, c v0.24.2,
cpp v0.23.4, css v0.25.0, elisp 1.6.1, go v0.25.0, html v0.23.2,
java v0.23.5, javascript v0.25.0, json v0.24.8, make v1.1.1,
markdown + markdown-inline v0.5.3, ocaml v0.25.0, python v0.25.0,
rust v0.24.2, toml v0.7.0, typescript + tsx v0.23.2, yaml v0.7.2.

Intersected with kg's current registry: C, Python, JavaScript, Rust,
Java, TypeScript (+TSX variant selection), HTML, Markdown (block
only), YAML have grammars; **Shell does not** (no tree-sitter-bash in
/opt-9), nor do Makefile, Lisp, C#, PHP, Ruby, Swift, SQL, Dart, or
the framework modes.  Modes without a grammar are plain text in a TS
build (decision 4's rule).  Shell was the one painful gap; the user
is adding `tree-sitter-grammar-{bash,elisp,make}` to the /opt-9
manifest (decided 2026-08-07).  Their tags are recorded here once the
installs exist, and Shell, Makefile and Lisp then join batch 2.
*(2026-08-08: the three installs exist.  Makefile and Lisp joined;
Shell did not — the bash install is v0.6.0 and unreadable.  See
"Landed" below.)*

- **Batch 1: C, Python, Markdown (block), YAML** — all four have
  bespoke scanners today, so the legacy backend is a behavioural
  sanity anchor while the differential test infrastructure comes up.
- **Batch 2: JavaScript, TypeScript/TSX, Java, Rust, HTML** (and any
  newly added grammars).  /opt-9 languages kg has no mode for (Go,
  JSON, TOML, OCaml, C++, CSS) are candidates for *new* cheap modes
  afterwards — the point of the whole exercise — but are not v1.

#### Landed (2026-08-08, slices 8 and 9)

Batches 1 and 2 are both in the registry: **thirteen rows over eleven
grammars** — C, Python, YAML, Markdown (block), JavaScript, React (the
javascript grammar again: tree-sitter-javascript parses JSX, so React
is not its own dependency), TypeScript and TSX (one mode, two rows,
chosen by file-name suffix, sharing one query text that is inside both
grammars' node inventories), Java, Rust, HTML, Emacs Lisp and Makefile.

The three grammars added to /opt-9 after this document was first
written resolve as follows.  `elisp` is **1.6.1** and `make` is
**v1.1.1** (both read out of the installed `.so` — there is no `.pc`
or `VERSION` file in those prefixes, so the tag comes from the build's
recorded source path).  `bash` is **v0.6.0**, which is *grammar ABI 6*
where tree-sitter 0.26.11 reads 13–15: kg's loader refuses it, so
**Shell has no registry row** and stays plain text.  That is the one
batch-2 target that did not land, and it needs a newer
tree-sitter-bash in /opt-9, not a change in kg — the row and its query
are a fifteen-minute follow-up once the pin moves.
`test_bash_grammar_abi_is_rejected()` is the assertion that will fail,
loudly and in the right place, on the day it does.

**Plain text by decision, not by omission**: Vue, Angular, Svelte, C#,
PHP, Ruby, Swift, SQL and Dart.  /opt-9 has no grammar for any of
them, and the three framework modes would additionally need language
injection (a second parser over a sub-range of one document), which is
out of v1 by Refinement decision 4.  Git commit and Git rebase stay
grammarless by policy: their C-c keys quit the editor, so their
behaviour must never depend on a third-party parser.

Two consequences of decision 4 are visible on screen rather than only
in the manifest: an HTML `<script>`/`<style>` body is one opaque
`(raw_text)` and is left uncoloured, and a Makefile recipe line is
shell and likewise plain, though the `$(...)` and `$@` references
inside it are coloured because they are the make grammar's own nodes.

The `.scm`-embedding generator this section sketches is **still not
built**, on second consideration rather than deferral — see the
reasoning block at the head of `src/syntax_tree_sitter_lang.c`.

### Corrections to the Response (verified against the tree)

- **Economics.**  The Response never mentions kg's ratchets.  Every
  phase here is priced and funded like the elisp program's: scc/
  pmccabe raises as Decisions with temporary-lowering proof, coverage
  floors for the new files, format/IWYU/header-check green per
  commit.  Splitting `src/syntax.c` (2123 lines today) rewrites the
  per-file complexity manifest; that re-set rides the split commit.
  Vendored/third-party sources stay out of all gates (Response is
  right there) — but nothing /opt-9-shaped enters the tree at all
  now, which shrinks that concern to the query `.inc` generator.
- **Phase 3's "audit of raw mutation paths" already exists** as
  `.ci/mutation-gateway.json`: today `src/buffer.c` (24 row
  primitives, 20 row-field writes) and `src/bufmgr.c` (3 row
  primitives), nothing else.  The prerequisite work consumes and
  drives that census (follow-up Plan 02's stated endpoint: buffer.c
  alone), not a fresh audit.
- **dired is a missed consumer.**  `src/dired.c:346` calls
  `kg_row_builder_highlight()` with its own local `dired_syntax`,
  exactly like the synthetic bufmgr modes.  The mode-identity
  registry must include it or dired breaks.
- **The perf gate collides in Phase 3, not Phase 12.**
  `test/test_perf.c` asserts counter shapes such as "exactly one
  `editor_update_row()` per logical replacement"; splitting render
  from syntax notification changes those shapes.  Updating the
  asserted shapes (deliberately, named in the commit) is part of the
  Phase 3 slice.
- **The feature stamp lives in `$(OBJDIR)`**, not `src/`:
  `Makefile:75` has `LISP_CONFIG = $(OBJDIR)/.with-lisp-$(WITH_LISP)`.
  The generalized `FEATURE_CONFIG` goes there, and the CI runner's
  copy/cleanup exclusions update in the same commit.
- **Re-derive the registry at authoring time.**  The "24 entries"
  list, `syntax.c` line numbers, and the gateway counts in this
  document are 2026-08-07 measurements; each sub-plan re-measures
  (the elisp program's Rule 6).

### Additions the Response lacks

- **Sanitizer lanes.**  ASan lane links the `/opt-9/...-asan` core;
  grammar `.so`s exist only as release builds, which ASan tolerates
  (it just cannot see into them).  **MSan cannot**: uninstrumented
  grammar code guarantees false positives, so the MSan lane builds
  `WITH_TREE_SITTER=0` — recorded as policy, not an accident.
  Valgrind runs the TS build normally.
- **Hosted CI.**  GitHub runners have no /opt-9.  TS lanes resolve
  `TREE_SITTER_PREFIX`/`KG_TS_GRAMMAR_PATH`; when absent the lane
  SKIPs with a printed reason (the suite's existing culture), and
  `--require-tools` turns that into a failure on boxes that should
  have it.  v1 keeps TS lanes developer-box-only; promoting them to
  hosted CI (building core+grammars from the pinned tags in a cached
  step) is a later, separate decision.
- **PTY gating.**  `requires_feature: tree-sitter` in PTY YAML,
  analogous to the existing `requires_feature: lisp` (kg `-V` gains
  `+tree-sitter`/`-tree-sitter` as the Response already proposes).
- **Latency policy.**  Incremental reparses are expected sub-ms; the
  first full parse of a large file happens once at load inside the
  staged-load transaction.  v1 parses synchronously with no timeout,
  but the bench suite measures parse wall time on the big corpora so
  the number is known before anyone feels it; a cancellation/budget
  mechanism (tree-sitter supports parse cancellation) is deferred
  until a measurement says it is needed — consistent with the C-g
  culture elsewhere.

### Prerequisites plan (to be authored first, after Phase 12)

The Response's Phases 1–4 are backend-neutral, behaviour-preserving
refactors valuable independently of tree-sitter, and they become
their own plan — working title **"syntax backend seam"** — authored
and landed with full green light *before* any tree-sitter sub-plan
exists.  Its scope, in this document's terms:

1. Mode identity (`kg_mode_id`) replacing highlighter-pointer
   comparisons; dired included; public registry lookup API replacing
   `test_syntax.c`'s `extern HLDB[]`.
2. The `syntax.c` → `syntax.c` + `syntax_legacy.c` split (source-list
   selection; per-file complexity manifest re-set rides the commit).
3. Edit-granular syntax notification (`kg_syntax_edit` staged in
   `kg_buffer_replace()`, `syntax_backend_rebuild()` for broad
   changes), consuming the mutation-gateway census and updating
   `test_perf`'s asserted shapes in the same slice.
4. Backend-preparable staged loads (opaque `kg_syntax_state`,
   ownership and release points as the Response lists).

Exit criterion: behaviour identical (full PTY suite, oracle cases,
perf shapes re-pinned), gateway census equal or decreased, caps
re-set at measured actuals — and the tree-sitter decision still
reversible at zero cost.  Tree-sitter sub-plans are then authored
against that re-measured baseline, roughly the Response's commit
sequence 5–11 with batch-1 grammars only.

---
