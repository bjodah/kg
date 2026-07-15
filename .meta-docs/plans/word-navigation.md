# Plan: Fix Word Navigation for Special Characters

## 1. Problem Description
The PTY test `word-navigation-special-chars.yaml` is failing. 
Currently, `kg` defines word boundaries purely based on whitespace characters using `isspace()`. Any non-whitespace character (including punctuation such as `*`, `[`, `]`, `{`, `}`, `(`, `)`, `"`) is treated as a valid word constituent. 

In the test case:
- `M-b` from `char *argv[]) {` lands immediately before `{`, skipping only the space because `{` is seen as a word.
- `M-d` then kills `{`.
- `M-f` from `printf("Hello world!");` treats `printf("Hello` as a single word because they are contiguous non-whitespace characters, jumping over both the function name and the string literal at once.

Emacs, on the other hand, distinguishes between whitespace, punctuation, and word constituents. A word is typically comprised of alphanumeric characters. 

## 2. Emacs Expected Behavior
- **`forward-word` (`M-f`)**: Move forward over any non-word characters (spaces AND punctuation), then move forward over all contiguous word characters until the end of the word is reached.
- **`backward-word` (`M-b`)**: Move backward over any non-word characters, then backward over all contiguous word characters until the start of the word is reached.
- **`kill-word` (`M-d`)**: Skip non-word characters forward, then skip word characters forward, killing the resulting region.
- **`backward-kill-word` (`M-DEL`)**: Skip non-word characters backward, then skip word characters backward, killing the resulting region.

## 3. Implementation Plan
We will refactor the logic in `src/word.c` to use alphanumeric boundaries instead of strictly whitespace boundaries.

### Step 3.1: Introduce Word Character Helper
In `src/word.c`, introduce a helper function/macro:
```c
static int is_word_char(int c) {
    return isalnum(c) || c == '_'; // Treat underscore as word constituent for practical purposes, or just isalnum(c).
}
```
*(Note: Emacs strictly uses `isalnum` for words and puts `_` in symbol syntax in C-mode, but depending on `kg`'s simplicity goals, either is an improvement over `!isspace`. Let's stick to `isalnum` or a configurable standard. For now, `isalnum` is the closest to Emacs `forward-word`)*

### Step 3.2: Update `editor_move_word_forward`
Change the two loops:
1. First `while` loop: Change `if (!isspace(...))` to `if (is_word_char(...))`. This skips spaces and punctuation.
2. Second `while` loop: Change `if (... || isspace(...))` to `if (... || !is_word_char(...))`. This skips the actual word characters.

### Step 3.3: Update `editor_move_word_backward`
Change the two loops identically:
1. `/* Skip whitespace */` becomes `/* Skip non-word chars */`. Condition `isspace(...)` becomes `!is_word_char(...)`.
2. `/* Skip word characters */` condition `!isspace(...)` becomes `is_word_char(...)`.

### Step 3.4: Update `editor_kill_word_forward` and `editor_kill_word_backward`
Apply the same `!is_word_char` / `is_word_char` logic to the skipping phases of these functions so that `M-d` and `M-DEL` accurately kill Emacs-style words.

### Step 3.5: Update Case Transformation Commands
The functions `editor_upcase_word`, `editor_downcase_word`, and `editor_capitalize_word` in `src/word.c` (if they exist) or similar logic might also rely on word boundaries. Ensure they use `is_word_char` to apply transformations only to the alpha characters.

## 4. Verification
After compiling, run `make check` (which evaluates `pty_accept.py` over all tests).
Specifically, ensure `FAIL: word-navigation-special-chars` becomes a `PASS`.
