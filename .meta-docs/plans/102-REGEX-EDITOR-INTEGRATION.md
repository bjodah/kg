# kg/.meta-docs/plans/102-REGEX-EDITOR-INTEGRATION.md

## Plan: Add regexp search and replacement to kg

### Goal

Add regex-powered editor commands to kg:

- `isearch-forward-regexp`
- `isearch-backward-regexp`
- eventually `query-replace-regexp`

These commands should use the shared `tiny-regex-c` engine and should work even when kg is built with `WITH_LISP=0`.

---

## Design decisions

### Use regex from C, not via Lisp evaluation

kg editor search should call a C wrapper around `tiny-regex-c`.

Do not evaluate Fe Lisp to perform editor search.

Rationale:

- search runs in tight interactive loops;
- kg search already owns cursor movement, highlight, prompt, and undo behavior;
- editor regex must work with `WITH_LISP=0`;
- Lisp evaluation adds avoidable GC/error/step-limit concerns.

### Keep tiny-regex-c under fe

For now, kg should compile:

```text
fe/tiny-regex-c/re.c
fe/tiny-regex-c/re.h
```

### Add kg wrapper layer

Add:
```
src/regex.c
src/regex.h
```

This wrapper owns kg-specific policy:

- smart-case;
- row-local matching;
- forward/backward search;
- zero-length match advancement;
- conversion from regex status to kg status messages.

## Phase 1: Build integration

### Makefile additions
Add a regex object independent of `WITH_LISP`.

Sketch:
```Makefile
REGEX_OBJ = $(OBJDIR)/re.o

$(OBJDIR)/re.o: fe/tiny-regex-c/re.c fe/tiny-regex-c/re.h
	$(CC) $(FE_CFLAGS) -Ife/tiny-regex-c -c $< -o $@

$(TARGET): $(OBJS) $(FE_OBJ) $(REGEX_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
```

Adjust:

- unit-test link lines;
- fuzz link lines if they include search.c or regex wrapper;
- `clean`;
- coverage builds.

If `WITH_LISP=0`, kg will still need the `fe` submodule present solely for `tiny-regex-c`, that is fine, do not overload `WITH_LISP` to disable regex.

## Phase 2: Add kg regex wrapper

### Proposed API
Names are provisional.

```C
enum kg_regex_status {
    KG_REGEX_OK = 0,
    KG_REGEX_NO_MATCH,
    KG_REGEX_BAD_PATTERN,
    KG_REGEX_TOO_COMPLEX,
    KG_REGEX_INTERNAL_ERROR
};

struct kg_regex_span {
    int start;
    int end;
};

struct kg_regex_match {
    int nspans;
    struct kg_regex_span spans[KG_REGEX_MAX_SPANS];
};

struct kg_regex {
    unsigned char storage[KG_REGEX_STORAGE_SIZE];
    unsigned storage_size;
    re_t compiled;
};
```

Functions:

```C
int kg_regex_compile(struct kg_regex *rx, const char *pattern, int flags);
int kg_regex_match_forward(
    const struct kg_regex *rx,
    const char *text,
    int start,
    struct kg_regex_match *out);

int kg_regex_match_backward(
    const struct kg_regex *rx,
    const char *text,
    int before,
    struct kg_regex_match *out);
```

### Semantics
#### Forward search:

- find first match whose start is at or after start;
- return whole-match span;
- allow zero-length matches;
- caller advances by at least one byte when iterating after zero-length matches.

#### Backward search:

- find last match whose start/end is before or at the requested boundary;
- avoid rediscovering the same zero-length match.

Indexes are byte offsets into the row string.

## Phase 3: Unit tests for kg regex wrapper
Add tests for:

- basic match;
- no match;
- invalid pattern;
- start offset;
- backward match;
- zero-length forward;
- zero-length backward;
- smart-case flag once available;
- match at end of row.

These tests should not require terminal/PTY tests.

## Phase 4: Refactor literal isearch into shared search mode
Current kg has literal incremental search. Refactor it to share UI with regexp search.
Sketch:

```C
enum search_kind {
    SEARCH_LITERAL,
    SEARCH_REGEXP,
};

static int isearch_find_match(
    enum search_kind kind,
    int start_row,
    int start_col,
    int direction,
    char *query,
    int qlen,
    int fold,
    int *match_row,
    int *match_col,
    int *match_len);
```

Literal search keeps existing behavior.
Regexp search calls kg regex wrapper.
Prompt examples:

```
I-search [fold]: foo
Regexp I-search [fold]: f.o
```

Invalid regex while typing:

```
Regexp I-search [bad regexp]: ...
```

Do not exit search on transient invalid patterns. Users often type temporarily invalid regexes.

## Phase 5: Add named commands
Add static commands:

```
isearch-forward-regexp
isearch-backward-regexp
```

Implementation:

```C
static void cmd_isearch_forward_regexp(int fd)
{
    editor_find_regexp(fd, 1);
}

static void cmd_isearch_backward_regexp(int fd)
{
    editor_find_regexp(fd, -1);
}
```

Register in command table.
Do not require keybindings initially. M-x command access is enough.

## Phase 6: Highlight whole regex matches
Use the whole-match span for highlighting.
Rules:

- if match length > 0, highlight the whole match;
- if match length == 0, use a visible zero-width indication if kg has one;
- if no zero-width highlight exists, move point but do not corrupt highlight memory.

For initial implementation, it is acceptable to show zero-length matches only by cursor movement/status message.

## Phase 7: Query replace regexp
Only start this after capture spans exist in tiny-regex-c.
Add named command:

```
query-replace-regexp
```

Behavior should mirror existing query replace:

- prompt for regexp;
- prompt for replacement;
- show each match;
- accept y, n, !, q, C-g;
- one undo record per accepted replacement.

Replacement expansion:

- \& → whole match;
- \1 ... \9 → capture groups;
- \\ → literal backslash.

Decide and test behavior for:

- missing captures;
- unknown escapes;
- zero-length matches;
- replacement that includes newlines;
- read-only buffers.

Initial regexp replacement may remain row-local.

## Phase 8: Keybindings
After M-x commands are stable, add keybindings if terminal input makes them reliable.
Targets:

- C-M-s / ESC C-s → isearch-forward-regexp
- C-M-r / ESC C-r → isearch-backward-regexp
- C-M-% / ESC C-% → query-replace-regexp

Because control-meta terminal encodings vary, implement and test keybindings separately from command behavior.

## Phase 9: Documentation
Update:

- README.md
- doc/kg.1
- help buffer if appropriate

Document:

- command names;
- supported regex syntax;
- smart-case behavior;
- row-local limitation;
- replacement escapes;
- differences from full Emacs regex.

## Non-goals

- Calling Fe Lisp for editor regex search.
- Full Emacs regex compatibility in the first pass.
- Multi-line regex.
- Unicode-aware matching.
- Capture-based replacement before engine capture spans exist.
