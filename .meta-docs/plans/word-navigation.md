# Plan: Fix Word Navigation for Special Characters

## 1. Problem Description
The PTY test `word-navigation-special-chars.yaml` is failing.
`kg` defines word boundaries purely via `isspace()`: every non-whitespace
character — including punctuation such as `*` `[` `]` `{` `}` `(` `)` `"`
`!` — counts as a word constituent.  Emacs instead separates whitespace from
punctuation and treats only word characters (alphanumeric) as a "word".

Concrete failures in the test (`helloworld.c`) with the current code:

- `M-b` from the end of `char *argv[]) {` lands at the start of the single
  whitespace-terminated run `argv[]) {`, i.e. before `argv`; so far this
  happens to coincide with Emacs.  But then:
- `M-d` kills the whole non-whitespace run `argv[]) {` (because the forward
  loop in `editor_kill_word_forward` stops only at whitespace), leaving
  `char *`.  Emacs kills just the word `argv` and stops at the punctuation
  `]`, leaving `char *[]) {`.
- `M-f` from the start of `    printf("Hello world!");` runs over
  `printf("Hello` as one word (contiguous non-whitespace), so it crosses both
  the function name and the string literal.  Emacs stops after `printf`.

## 2. Emacs Expected Behavior
A "word character" for `forward-word`/`backward-word` is alphanumeric.
Underscore is a *symbol constituent* in Emacs C mode (not a word char), but
`kg` has only one global grammar, so we treat `_` as a word constituent too
to match common editor behaviour for identifiers; this keeps all existing
tests (`abc def ghi`) green and is strictly better than `!isspace`.

- **`forward-word` (`M-f`)**: skip non-word characters (whitespace **and**
  punctuation), then skip contiguous word characters; point lands after the
  word.
- **`backward-word` (`M-b`)**: step back one, skip non-word characters
  backward, then skip contiguous word characters backward; point lands before
  the word.
- **`kill-word` (`M-d`)**: same skip phases as `M-f`, killing the region from
  the original point to the final point (current line only — never crosses
  newlines, matching the existing single-line restriction).
- **`backward-kill-word` (`M-DEL`)**: mirror of `M-b`, killing the backward
  region (current line only).
- **`upcase/downcase/capitalize-word` (`M-u`/`M-l`/`M-c`)**: transform the
  word forward from point, bounded by word characters (not non-whitespace).
  Capitalize uppercases only the first alphabetic character and lowercases
  the rest of the word.
- `zap-to-char` (`M-z`) and sentence/paragraph motion are **not** word-based
  and must not change.

## 3. Implementation Plan
Refactor `src/word.c` to distinguish word characters from punctuation.

### 3.1 Word-character helper
Add near the top of `src/word.c`:
```c
static int is_word_char(int c)
{
	return isalnum((unsigned char)c) || c == '_';
}
```
Using `(unsigned char)` cast matches the existing `isspace` calls and keeps
8-bit bytes well-defined.

### 3.2 `editor_move_word_forward`
- First loop (skip non-word): `if (!isspace(...)) break;` → `if (is_word_char(...)) break;`
  (it then stops on the first word char; whitespace and punctuation are both
  skipped).  This loop already crosses newlines via the `filecol >= row->size`
  fall-through; keep that behaviour unchanged.
- Second loop (skip word): `|| isspace(...)` → `|| !is_word_char(...)`.

### 3.3 `editor_move_word_backward`
- After the initial one-step-left, the "Skip whitespace" loop becomes
  "Skip non-word": `isspace(row->chars[filecol])` → `!is_word_char(...)`.
- The "Skip word characters" loop: `!isspace(row->chars[filecol - 1])` →
  `is_word_char(row->chars[filecol - 1])`.

### 3.4 `editor_kill_word_forward` (`M-d`)
- When the char at point is a word character: skip word characters forward
  (stop at first non-word).
- When it is non-word (space or punctuation): skip non-word characters
  forward, **then** skip word characters forward (so `M-d` from before `argv`
  kills `argv`; `M-d` from `{` skips `{` + any whitespace then the following
  word).
- Keep the single-line restriction (never advance past `row->size`).
- Concretely, replace the `isspace`/`!isspace` branches:
  - leading `isspace` branch → `!is_word_char` branch (skip non-word run, then
    a word run);
  - the `else` branch currently skips one non-space run then a space run;
    change it to: if at a word char skip the word run; if at a non-word char
    skip the non-word run (then optionally a following word run, matching
    Emacs `kill-word` which always advances at least one full word).

### 3.5 `editor_kill_word_backward` (`M-DEL`)
Mirror 3.3: skip non-word chars backward, then word chars backward, both
within `filecol > 0`.

### 3.6 `do_word_case` (`M-u`/`M-l`/`M-c`)
- The `word_start` advance (`isspace` → skip whitespace) should skip *non-word*
  chars forward (`!is_word_char`).
- The `word_end` advance (`!isspace` → run to end of word) should advance over
  *word* chars only (`is_word_char`).
- Capitalize: keep `(i == 0) ? toupper : tolower` (already correct once
  `word_end` marks the real word end).
- This keeps `M-c` on `hello` → `Hello` and makes `M-u` on `printf` under
  point work and stop at `(`, whereas previously it would have overrun into
  `("Hello`.

## 4. Verification
1. `make` then `python3 utils/pty_accept.py --kg src/kg \
   test/pty/word-navigation-special-chars.yaml` must PASS (expect leaves
   `char *[]) {` on line 1 and `printf("");` on line 2).
2. Regression: the existing isearch handoff cases
   `test/pty/07-isearch-handoff-alt-b.yaml` and
   `test/pty/21-isearch-handoff-set-mark.yaml` must still PASS (they use pure
   alpha words, so behaviour is unchanged).
3. `make check` (native + all PTY cases) stays green; no native
   word-movement unit test regresses.

## 5. Notes / Risks
- Unrecognised trailing bytes after a failed parse are reachable by the
  dispatcher as a bare `ESC` plus the next key (Alt-prefix), so the change must
  not alter behaviour for plain ASCII words — it does not, because `is_word_char`
  reduces to "non-whitespace" only for punctuation/whitespace distinctions.
- The existing `editor_move_word_forward` newline-crossing in the first loop
  is intentionally preserved (Emacs `M-f` does cross newlines); the kill
  commands stay single-line by design.
- Do not change `editor_move_paragraph_*`, sentence motion, or `zap-to-char`.