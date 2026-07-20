# Plan: add a lightweight `yaml-mode`

## Prerequisite

This plan depends on **`syntax-mode-framework.md`**, which must land first. That
plan extracts the shared mode plumbing YAML would otherwise have to hand-roll:

* data-driven dispatch via an `editor_syntax.highlight` function pointer (so
  YAML adds a table row + one function, with no edits to
  `editor_update_syntax()` and no `SHL_YAML` dispatch flag);
* one centralized, iterative cross-row `hl_oc` propagation helper (so YAML block
  scalars do not re-implement "re-highlight the next row");
* empty-row dispatch to stateful modes (so block scalars survive blank lines);
* `syntax_find_by_name()` / `editor_set_syntax()` / `editor_rehighlight_all()`
  (so `M-x yaml-mode` rebuilds existing rows correctly);
* name-based `HLDB` lookup in tests (so a new entry never touches an index
  literal).

With that in place the sections below shrink to genuinely YAML-specific work.
Where a step is fully provided by the framework it is marked *(framework)* and
kept only as a pointer.

## Recommended scope

Implement a **YAML-aware syntax highlighter**, not a YAML parser. The first version should provide:

* Automatic selection for `.yaml` and `.yml`.
* `M-x yaml-mode` for extensionless files.
* Highlighting for:

  * mapping keys;
  * comments;
  * quoted strings;
  * block scalars (`|` and `>`);
  * booleans, nulls and numbers;
  * sequence markers;
  * document markers;
  * anchors, aliases and tags.
* Correct highlighting across multiline block scalars and blank lines.

It should explicitly **not** try to provide:

* YAML validation;
* schema inference;
* formatting;
* structural navigation;
* GitHub Actions, Helm or Jinja template parsing;
* automatic indentation changes.

The existing generic highlighter is table-driven and is designed around fixed keyword lists. Its own documentation says pattern highlighting is unsupported. YAML keys are arbitrary, and block scalars need indentation-sensitive multiline state, so YAML warrants a dedicated highlighter like the existing Markdown and Makefile paths.

---

## 1. Register YAML as a specialized syntax

### `src/syntax.c`

Add the extension list and an `HLDB` entry that points `highlight` at
`yaml_syntax` (the framework's function-pointer dispatch, see
`syntax-mode-framework.md` §1):

```c
static char *YAML_HL_extensions[] = { ".yaml", ".yml", NULL };

/* in HLDB, with the trailing .highlight field the framework added: */
{
    "YAML", YAML_HL_extensions, NULL,
    "#", "", "",
    0,            /* no dispatch flag needed */
    yaml_syntax,  /* highlighter */
},
```

No `SHL_YAML` flag and no edit to `editor_update_syntax()` are required — the
framework dispatches on the pointer. Filename matching already supports
dot-prefixed suffixes, so `.yaml`/`.yml` are selected automatically.

`erow.hl_oc` is already the framework's per-row "trailing state" (its comment is
updated by the framework plan). It is an `int`, so YAML can pack a state kind
and an indentation level into it (see §5) without changing the row layout.

---

## 2. The YAML highlighter (`yaml_syntax`)

`yaml_syntax()` is the single per-row entry the framework calls. It must:

* tolerate a zero-length row (state propagation only — the framework now
  dispatches empty rows too);
* set `row->hl` for the row's tokens;
* set `row->hl_oc` to the trailing state it forwards, and **not** chase the
  next row itself (the framework's centralized propagation does that).

Do not add YAML behaviour to the generic scanner. Its assumptions differ from YAML in several important ways:

* YAML keys are not a fixed keyword set.
* Single-quoted YAML strings escape `'` as `''`, not with backslashes.
* `:` has structural meaning only in certain contexts.
* `#` is a comment marker only outside quoted scalars and with the appropriate separation.
* `|` and `>` introduce indentation-sensitive multiline content.

Keeping those rules in `yaml_syntax()` avoids destabilizing every existing language.

---

## 3. Split the YAML scanner into small helpers

Avoid one large `yaml_syntax()` function. A reasonable decomposition is:

```c
static int yaml_indent(const erow *row);
static int yaml_is_comment_start(const char *p, int pos);
static int yaml_find_mapping_colon(const char *p, int start, int end);
static int yaml_scan_single_quoted(erow *row, int start);
static int yaml_scan_double_quoted(erow *row, int start);
static int yaml_scan_anchor_or_tag(erow *row, int start);
static int yaml_scan_number(erow *row, int start);
static int yaml_scalar_keyword_length(const char *p, int remaining);
static int yaml_block_indicator(const char *p, int start, int end,
    int *explicit_indent);
static void yaml_syntax(erow *row);
```

The top-level function should primarily coordinate state and scanning.

---

## 4. Define the highlighting rules

The existing token classes are sufficient; no new terminal colours are needed. Keys can use `HL_KEYWORD1`, structural/special tokens `HL_KEYWORD2`, quoted and block scalars `HL_STRING`, numbers `HL_NUMBER`, and comments `HL_COMMENT`. Those currently map to yellow, green, magenta, red and cyan respectively.

### Mapping keys

Highlight a mapping key as `HL_KEYWORD1`.

For block style:

```yaml
server:
  host: localhost
  port: 8080
```

Recognize a colon as a mapping separator only when:

* it is outside quoted strings;
* it is followed by whitespace, end-of-line, `,`, `}` or `]`;
* the current scanner position permits a mapping key.

That avoids treating these as key separators:

```yaml
url: http://example.com
time: 12:34
```

Support keys after a sequence marker:

```yaml
- name: first
  enabled: true
```

For the first version, support these key forms:

```yaml
plain-key:
"quoted key":
'single quoted key':
<<:
```

Quoted key content should remain `HL_STRING`; the surrounding structural context can still recognize the colon.

### Comments

Recognize `#` outside quotes when it appears:

* at the first non-indentation position; or
* after whitespace.

Examples:

```yaml
# full-line comment
key: value  # trailing comment
literal: abc#def
quoted: "not # a comment"
```

The first two should be comments. The latter two should not.

### Quoted scalars

Double-quoted scalars:

* Highlight the full scalar as `HL_STRING`.
* Treat backslash plus the following byte as escaped.
* Do not let `#`, `:` or block indicators terminate it.

Single-quoted scalars:

* Highlight the full scalar as `HL_STRING`.
* Treat `''` as an embedded quote.
* Do not treat backslash specially.

Basic multiline quoted-scalar state can be deferred to a second pass, but the state encoding should leave room for it.

### Core-schema values

Highlight these as `HL_KEYWORD2` when they form a complete scalar token:

```yaml
true
True
TRUE
false
False
FALSE
null
Null
NULL
~
```

I recommend following YAML 1.2-style values and **not** treating `yes`, `no`, `on` and `off` as booleans. Those YAML 1.1 spellings frequently cause ecosystem-dependent surprises.

Quoted forms remain strings:

```yaml
enabled: true
label: "true"
```

### Numbers

Highlight a complete numeric token as `HL_NUMBER`, including useful YAML forms:

```yaml
42
-17
3.14
1.0e-6
0x2a
0o52
.inf
-.inf
.nan
```

Implement this as token classification rather than reusing the generic character-by-character number logic. This prevents partially highlighted malformed values and makes exponent handling predictable.

### Structural tokens

Use `HL_KEYWORD2` for:

```yaml
---
...
-
&anchor
*alias
!tag
!!str
```

Rules:

* `---` and `...` only count at the first non-indentation position and must be followed by whitespace or end-of-line.
* `-` is a sequence marker only at the logical line start and followed by whitespace or end-of-line.
* `&`, `*` and `!` consume a token until whitespace or flow punctuation.
* `%YAML` and `%TAG` directives can use `HL_KEYWORD1`.

Flow punctuation such as `{}`, `[]`, `,` and `:` can remain `HL_NORMAL`. Colouring all punctuation generally makes YAML noisier rather than clearer.

---

## 5. Handle block scalars with row state

This is the part that makes a dedicated mode worthwhile:

```yaml
script: |
  echo first

  echo second
next: value
```

The body, including the blank line, should remain `HL_STRING`, while `next:` must return to ordinary YAML parsing.

### Suggested state encoding

Use `hl_oc` as a bit field:

```c
#define YAML_STATE_KIND_MASK       0xff000000
#define YAML_STATE_INDENT_MASK     0x00ffffff

#define YAML_STATE_NONE            0x00000000
#define YAML_STATE_BLOCK_PENDING   0x01000000
#define YAML_STATE_BLOCK_CONTENT   0x02000000
#define YAML_STATE_SINGLE_QUOTE    0x03000000
#define YAML_STATE_DOUBLE_QUOTE    0x04000000
```

For block scalars:

* `BLOCK_PENDING | header_indent` means a `|` or `>` was encountered, but the content indentation has not yet been inferred.
* `BLOCK_CONTENT | content_indent` means subsequent nonblank rows at or beyond that indentation are scalar content.

### Block indicator parsing

Recognize:

```yaml
|
|-
|+
|2
|2-
|+2
>
>-
>+
>4
```

The chomping modifier does not affect highlighting.

When an explicit indentation indicator exists, content indentation is:

```text
header indentation + explicit indentation
```

Otherwise, infer it from the first nonblank content row.

### Continuation algorithm

For each new row:

1. Read the previous rows YAML state.
2. When state is `BLOCK_PENDING`:

   * blank row: preserve the pending state;
   * deeper-indented nonblank row: highlight it as `HL_STRING` and establish its indentation as the content indentation;
   * same or shallower indentation: terminate the block and parse the row normally.
3. When state is `BLOCK_CONTENT`:

   * blank row: highlight nothing but preserve block state;
   * sufficiently indented row: mark the scalar body as `HL_STRING`;
   * dedented row: terminate block state and parse normally.

Leading indentation can remain `HL_NORMAL`; only scalar content needs `HL_STRING`.

---

## 6. Empty-row and cross-row state *(framework)*

Both are provided by `syntax-mode-framework.md`:

* Empty rows are now dispatched to stateful highlighters (framework §3), so
  `yaml_syntax()` sees blank lines and can keep a block scalar alive across
  them — it just has to tolerate `rsize == 0`.
* Cross-row propagation is centralized and iterative (framework §2).
  `yaml_syntax()` only sets `row->hl_oc`; the framework re-highlights following
  rows when that state changes.

The YAML-specific requirement is that `hl_oc` correctly encodes the trailing
state so these framework mechanisms fire in the right cases:

* changing `|` to an ordinary scalar;
* changing block content indentation;
* inserting or deleting a blank line inside a block;
* converting a block-body line into a dedented mapping key;
* changing an explicit indentation indicator such as `|2`.

These belong in the block-scalar tests (§8, "Block scalars").

---

## 7. Register `yaml-mode`

The major-mode setter is provided by the framework
(`editor_set_syntax()` / `syntax_find_by_name()`). YAML only adds the command:

```c
static void cmd_yaml_mode(int fd)
{
    (void)fd;
    editor_set_syntax(syntax_find_by_name("YAML"));
    editor_set_status_message("YAML mode enabled");
}
```

Register it in the command table:

```c
{ "yaml-mode", cmd_yaml_mode, CMD_NONE },
```

Selecting the `YAML` syntax shows `YAML` in the mode line via
`editor_syntax.name`; `editor_set_syntax()` rebuilds existing rows' highlight
arrays.

---

## 8. Tests

Test lookup is name-based (framework §5), so YAML tests use
`syntax_find_by_name("YAML")` rather than a numeric `HLDB` index.

### Essential YAML unit tests

Add focused cases for:

1. **Selection**

   * `config.yaml`
   * `config.yml`
   * unrelated extension remains unchanged.

2. **Keys**

   * `name: value`
   * `- name: value`
   * quoted key;
   * merge key `<<:`;
   * no false key in `http://` or `12:34`.

3. **Comments**

   * full-line;
   * trailing;
   * `abc#def`;
   * `"# not comment"`.

4. **Strings**

   * double-quoted escape;
   * single-quoted doubled quote;
   * colon and hash inside strings.

5. **Scalars**

   * booleans;
   * null and `~`;
   * integers, floats, exponents, hex and octal;
   * quoted boolean remains a string.

6. **Special tokens**

   * `---`, `...`;
   * `&anchor`, `*alias`, `!tag`;
   * `%YAML 1.2`.

7. **Block scalars**

   * literal `|`;
   * folded `>`;
   * chomping modifiers;
   * explicit indentation;
   * blank lines inside a block;
   * dedent ends a block;
   * editing the block header re-highlights following rows.

8. **Safety**

   * lone quote;
   * lone `&`, `*`, `!`, `|` or `>`;
   * colon at end of short rows;
   * empty rows;
   * malformed escape at end-of-line.

The malformed-input cases are especially important for preventing the out-of-bounds class already covered by Markdown regression tests.

A small PTY test can verify that `M-x yaml-mode` changes the mode-line name, but the bulk of this work belongs in `test_syntax.c`.

---

## 9. Documentation

Update:

* `README.md`: list YAML among supported syntax modes.
* `doc/kg.1`: document `.yaml`/`.yml` auto-detection and `M-x yaml-mode`.
* Built-in help if named major modes are listed there.

Suggested wording:

> YAML files ending in `.yaml` or `.yml` use YAML syntax highlighting automatically. `M-x yaml-mode` enables it for the current buffer manually. Highlighting is lexical and does not validate YAML.

---

## Suggested commit sequence

**Prerequisite:** land `syntax-mode-framework.md` in full first. The steps
below assume the function-pointer dispatch, centralized propagation, empty-row
dispatch, `editor_set_syntax()`/`syntax_find_by_name()` and name-based test
lookup already exist.

1. **Add basic YAML highlighting**

   * Extensions and HLDB entry (with the `highlight` pointer).
   * Keys, comments, quoted strings, scalar values, markers, tags and anchors.
   * Unit tests for single-line constructs.

2. **Add YAML multiline state**

   * Block scalar indicators.
   * Indentation-state encoding in `hl_oc`.
   * Block-scalar behavior across blank lines and dedents.
   * Block-scalar edit regression tests (relies on framework propagation).

3. **Expose and document `yaml-mode`**

   * Named command via `editor_set_syntax()`.
   * Help, man page and README.
   * Optional PTY test.

The most important design decision is to make block scalars part of the initial implementation. Without them, ordinary CI configurations and Compose files would frequently show large script bodies as incorrectly parsed YAML structure,
 making the mode look unreliable despite handling simpler files correctly.
