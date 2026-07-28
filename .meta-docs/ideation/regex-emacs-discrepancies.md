# Reconnaissance: GNU Emacs vs `fe/tiny-regex-c` regex discrepancies

Raw findings only. No fixes proposed, no code changed.

Oracle: GNU Emacs 31.0.90 (`/opt-3/emacs-31-lucid/bin/emacs -Q --batch`),
`string-match` + `match-beginning`/`match-end`, `case-fold-search` set
explicitly per case, offsets reported both as characters and as UTF-8 bytes so
they compare directly against the byte-oriented engine.

Engine under test: `fe/tiny-regex-c/re.c` via `re_compile_checked()` +
`re_exec()` — exactly the entry points `src/regex.c` uses.

Harness (scratch, outside the tree):
`/tmp/claude-0/-work/8d968ca5-6510-467b-b727-07ee929d9848/scratchpad/rxprobe/`

* `driver.c` / `driver` / `driver_asan` — hex-encoded `<pattern> <text> <icase>
  [start_offset]` in, `OK <start> <len> [G1=..]` / `NOMATCH` / `BADPAT` /
  `TOOCOMPLEX` out. Built plain and with `-fsanitize=address,undefined`.
* `oracle.el` — the Emacs side, same hex protocol.
* `harness.py`, `fuzz.py`, `reduce.py`, `red2.py`, `reduce_over.py` —
  batch comparison, randomised differential fuzzing, delta-debugging.
  Every batch runs under a timeout; a stalled batch is split so the offending
  case is attributed rather than retried.
* `repro_kg.c` — ASan proof of the heap overread in kg's own call path.
* `overshoot.yaml`, `utf8.yaml`, `caret.yaml` — end-to-end PTY demonstrations
  driven with `utils/pty_accept.py` (kept out of `test/`).

Harness was validated first: `abc`/`xabcx`, `a\|b`, `\(ab\)+`, `a\{2\}`,
`[abc]+`, `^abc`, `abc$`, `a.c`, ICASE on/off all agree byte-for-byte before
any finding below was trusted.

Reachability column: **IS** = `isearch-forward-regexp` /
`isearch-backward-regexp` (matches `row->render`), **QR** =
`query-replace-regexp` (matches and *edits* `row->chars`).

---

## Severity summary

| Category | Count | Notes |
|---|---|---|
| **B** memory safety | 1 family | over-long span → heap overread + wrong text deleted; ASan + PTY proof |
| **A** claims support, wrong answer | 10 | alternation scope and group/interval backtracking are the big two |
| **D** unsupported but silently misparsed | 13 | intervals silently becoming literal text is the nastiest |
| **E** byte-vs-character | 7 | `.`/`[^x]`/quantifiers split UTF-8 glyphs; PTY-proven file corruption |
| **C** unsupported, fails cleanly | 5 | noted only |

**Single highest-priority item: B1** — the engine can report a match whose end
lies past the end of the subject string (up to +22 bytes observed). In
`query-replace-regexp` that is both a heap-buffer-overflow read and silent
destruction of text outside the match.

---

## B — crash / OOB / hang

### B1. Match span can end beyond the end of the subject

The reported `spans[0].end` can exceed `strlen(text)`.

Minimal cases (all `icase=0`):

| pattern | subject | len | tiny-regex-c | Emacs |
|---|---|---|---|---|
| `a*.c+` | `ac` | 2 | `OK 0 3` (end 3 > 2) | `OK 0 2` |
| `[a-c]+\w\{2\}b` | `baab` | 4 | `OK 0 6` (+2) | `OK 0 4` |
| `\(.*.a\{2\}\)` | `baa` | 3 | `OK 0 5` (+2) | `OK 0 3` |
| `\w*.\{3\}a?[a-c]\{1\}` | `b11bZZZ` | 7 | `OK 0 16` (+9) | `OK 0 4` |

Rate: 149 / 40000 (0.37%) randomly generated patterns over the *documented*
grammar produce an out-of-range span; maximum overshoot observed **+22 bytes**.
The pattern always involves a greedy `*`/`+` or an interval followed by more
pattern; matchlength appears to be accumulated twice on some backtracking
paths (`matchplus()` adds `text - prepoint` on top of the increments already
made by the recursive `matchpattern()`).

**Memory safety, kg's own path.** `src/search.c:845` does

```c
undo_push(UNDO_REPLACE_TEXT, filerow, match_start, expanded_len,
          row->chars + match_start, match_len);
```

and `undo_push()` (`src/undo.c:73`) does `memcpy(op->text, text, len)`.
`row->chars` is a `malloc(size + 1)` allocation, so a span overshooting by 2 or
more reads off the end of the heap block. Proof (`repro_kg.c`, links
`src/regex.c` + `re.c`, ASan):

```
$ ./repro_kg '\(.*.a\{2\}\)' 'baa'
==1754677==ERROR: AddressSanitizer: heap-buffer-overflow
READ of size 5 at 0x502000000014
0x502000000014 is located 0 bytes after 4-byte region
```

**Data loss, end to end.** `overshoot.yaml`, real `src/kg` under
`utils/pty_accept.py`: a line `b11bZZZ`, `M-x query-replace-regexp`,
pattern `\w*.\{3\}a?[a-c]\{1\}`, replacement `X`, `!`:

```
--- expected (Emacs semantics)
+++ actual (kg)
-XZZZ
+X
```

kg deleted the entire line; `ZZZ` was never part of any match. The deletion
loop `for (i = 0; i < match_len; i++) editor_row_del_char(row, match_start);`
is itself bounds-checked, so it truncates rather than corrupting memory — but
it removes characters outside the match.

Reachable: **QR** (destructive), **IS** (highlight/cursor land past the match;
the highlight `memset` is guarded by `rcol_start + rcol_len <= row->rsize`, so
IS is cosmetic only).

### B-negative: no crashes, hangs or sanitizer reports in the engine itself

* 20 000 malformed/adversarial patterns (`[`, `[^`, `\`, `\{`, `a\{2,1\}`,
  `\{99999\}`, `((((`, stray `\)`, `[[:`, `\x` truncations, raw 0x80/0xff
  bytes, mixed with valid syntax) × random subjects, under
  `-fsanitize=address,undefined`: **0 reports, 0 timeouts**.
* 2 000 long patterns (up to 3 000 fragments, deliberately straddling
  `RE_MAX_COMPILED_BYTES`), same sanitizers: **0 reports**.
* ReDoS: `a.*.*.*.*.*.*b`, `\(a*\)*b`, `\(a\|a\)*b`, `.*.*.*.*.*.*.*x` on
  60–3000 byte subjects all return in ≤0.02 s. `MAX_MATCH_STEPS` does its job;
  I found no hang. (But see D13 for how the budget is *reported*.)

The engine's internal bounds handling looks solid. The memory-safety problem is
entirely in what it *reports* to the caller.

---

## A — claims support, silently wrong answer

### A1. `\|` binds only the single preceding atom (left) but the whole remainder (right)

`ab\|cd` is parsed as `a\(b\|cd\)`, not `\(ab\)\|\(cd\)`.
`matchbranch()` receives only the node immediately before `BRANCH` as the left
alternative, and returns success after consuming *one* character.

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `ab\|cd` | `cd` | NOMATCH | `OK 0 2` |
| `foo\|bar` | `bar` | NOMATCH | `OK 0 3` |
| `abc\|abd` | `abd` | NOMATCH | `OK 0 3` |
| `za\|b` | `zb` | **`OK 0 2`** | **`OK 1 1`** |
| `\(ab\)\|\(cd\)` | `cd` | NOMATCH | `OK 0 2` |
| `\(a\)\|b` | `b` | NOMATCH | `OK 0 1` |
| `x\(ab\|cd\)y` | `xcdy` | NOMATCH | `OK 0 4` |
| `\(ab\|cd\)ef` | `cdef` | NOMATCH | `OK 0 4` |
| `\(a\|ab\)c` | `abc` | NOMATCH | `OK 0 3` |

`za\|b` on `zb` is the worst shape: both engines match, at different offsets
and lengths — in **QR** that replaces the wrong two characters.
Single-character alternatives (`a\|b`, `a\|b\|c`, `\(a\|b\)c`) work, which is
exactly why this is easy to miss.

Reachable: **IS**, **QR**. `\|` is documented in `re.h`.

### A2. Interval quantifiers never backtrack

`matchtimes*()` greedily consume via `matchcount()` and never give characters
back.

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `.\{2,3\}c` | `abc` | NOMATCH | `OK 0 3` |
| `a\{2,3\}a` | `aaa` | NOMATCH | `OK 0 3` |
| `a\{2,\}a` | `aaa` | NOMATCH | `OK 0 3` |
| `a\{,3\}a` | `aaa` | NOMATCH | `OK 0 3` |

`*` and `+` on a plain atom *do* backtrack correctly (`a*a`, `a+a`, `.*c`,
`[0-9]*5` all agree), so the failure is specific to `\{...\}`.

Reachable: **IS**, **QR**.

### A3. A quantifier inside `\(...\)` cannot give back to satisfy what follows the group

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `\(.*\)x` | `yx` | NOMATCH | `OK 0 2` G1=0,1 |
| `\(a*\)a` | `aa` | NOMATCH | `OK 0 2` G1=0,1 |
| `\(a+\)a` | `aa` | NOMATCH | `OK 0 2` G1=0,1 |
| `\(.+\)c` | `bc` | NOMATCH | `OK 0 2` G1=0,1 |
| `\(a\{2\}\)a` | `aaa` | NOMATCH | `OK 0 3` |
| `^\(.*\),\(.*\)$` | `a,b` | NOMATCH | `OK 0 3` G1=0,1 G2=2,3 |

The last row is the canonical field-swapping `query-replace-regexp` pattern, so
this is not an exotic shape. Note `\(a*\)b` on `ab` *does* work — the group only
fails when the text after the group can also be consumed by the group's own
quantifier. `matchgroup()`'s comment already documents that it gives up
resuming when the group's content resolves via `*`, `+`, `?` or `|`.

Reachable: **IS**, **QR**.

### A4. `?` is non-greedy

`re.h` documents `'?' Question, match zero or one (non-greedy)`. Emacs `?` is
greedy. This is a deliberate documented choice in the header but an outright
Emacs incompatibility, and it silently shortens matches.

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `a?` | `a` | `OK 0 0` | `OK 0 1` |
| `ab?` | `ab` | `OK 0 1` | `OK 0 2` |
| `[ab][ab]?` | `baab1acc` | `OK 0 1` | `OK 0 2` |
| `c[^a]?` | `bc1cacc.` | `OK 1 1` | `OK 1 2` |

Every diff in a 250-case fuzz run over `.`/classes/`*`/`+`/`?`/groups was this
one cause (8/250); with `?` removed from the generator the same subset produced
**0** diffs over 250 cases. Ambiguous which is "right" only in the sense that
the header documents the current behaviour — as an Emacs dialect it is wrong.

Reachable: **IS**, **QR**.

### A5. `^`/`$` inside a group, or anywhere but the pattern edges, never match

`END` is only honoured when the next node is the terminator, and `matchone()`
returns 0 for `BEGIN`; a `$`/`^` node anywhere else compares against
`u.ch == 0`, which no byte in a NUL-terminated string can be.

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `\(a$\)` | `ba` | NOMATCH | `OK 1 1` G1=1,2 |
| `\(^a\)` | `ab` | NOMATCH | `OK 0 1` G1=0,1 |
| `a$\|b` | `a` | NOMATCH | `OK 0 1` |
| `^\|a` | `xa` | NOMATCH | `OK 0 0` |

`\(a\)$` and `^\(a\)` (anchor outside the group) are fine. Both `\(...\)` and
`^`/`$` are documented as supported, so the combination is category A.
See also D10 for the literal-`$`/`^` cases.

Reachable: **IS**, **QR**.

### A6. `MAX_GROUP_REPEATS` (256) silently truncates instead of failing

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `\(a\)*` | 300 × `a` | `OK 0 256` | `OK 0 300` |
| `\(ab\)*` | 300 × `ab` | `OK 0 512` | `OK 0 600` |
| `\(a\)\{257\}` | 257 × `a` | NOMATCH | `OK 0 257` |
| `\(a\)\{300\}` | 300 × `a` | NOMATCH | `OK 0 300` |

`a*` on 300 `a` is fine (the cap is per quantified *group*). A truncated match
in **QR** replaces part of the intended text and leaves the tail behind.
kg rows can easily exceed 256 characters.

### A7. `^` matches at `re_exec`'s `start_offset`, not only at the true start of the string

`re_matchp_internal()` anchors to `text + start_offset`, so a `BEGIN` node
succeeds wherever the caller started scanning.

```
$ printf '5e61 786161 0 1\n' | ./driver     # pattern "^a", text "xaa", offset 1
OK 1 1
$ emacs -Q --batch --eval '(string-match "^a" "xaa" 1)'  =>  nil
```

kg passes a non-zero offset in both entry points: `src/search.c:96`
(`col = start_col`, and `col = last_match_col + 1` on every `C-s` repeat) and
`src/search.c:766` (`match_col`). So a `^`-anchored regexp search matches at the
cursor column, and `query-replace-regexp` re-anchors after every replacement.

End-to-end (`caret.yaml`, real `src/kg`): buffer `za` / `aq`, cursor moved one
column right, `M-x isearch-forward-regexp` `^a`:

```
--- expected (Emacs: only line 2 starts with "a")
+++ actual (kg: matched inside line 1 at the cursor)
-za        +zaZ
-Zaq       +aq
```

This one lives in `src/regex.c`/`src/search.c` rather than the engine — the
engine has no way to distinguish "start scanning here" from "the string starts
here". Whichever layer owns it, the user-visible behaviour is wrong.

Reachable: **IS**, **QR**.

### A8. Patterns that overflow `RE_MAX_COMPILED_BYTES` are silently truncated, not rejected

`re_compile_to()` exits its loop when the buffer fills, writes the `UNUSED`
sentinel, and returns a valid pointer; `re_compile_checked()` reports
`RE_STATUS_OK`.

`a` × 2000 against `a` × 2000 → `OK 0 1365`: the pattern was silently cut to
1365 atoms. Emacs matches 2000.

Not reachable from kg's UI (`KILO_QUERY_LEN` is 256, so ~256 atoms max), but it
is reachable from the Fe Lisp binding and from any future caller. Worth noting
because `RE_STATUS_BUFFER_TOO_SMALL` exists and is not used here.

### A9. ICASE lowercases both ends of a character range

`matchrange()` applies `tolower()` to both range endpoints, which collapses
ranges that span the ASCII case gap.

| pattern | subject | icase | tiny | Emacs |
|---|---|---|---|---|
| `[Z-a]` | `_` | 1 | NOMATCH | `OK 0 1` |
| `[Z-a]` | `_` | 0 | `OK 0 1` | `OK 0 1` |

Low impact, but it means a pattern can match *fewer* things when case folding
is enabled. kg enables ICASE automatically whenever the query has no uppercase.

### A10. Leading `]` in a bracket expression is not a literal member

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `[]]` | `]` | NOMATCH | `OK 0 1` |
| `[]a]` | `a` | NOMATCH | `OK 0 1` |
| `[^]]` | `]a` | NOMATCH | `OK 1 1` |

`[]]` is the only portable way to put `]` in a class (see also D9: the `[\]]`
spelling means something different in Emacs). Both spellings therefore
disagree, so there is currently *no* spelling of "a class containing `]`" that
behaves the same in both.

---

## D — unsupported but silently MIS-PARSED

### D1. Interval forms the parser rejects silently become literal text

When `sscanf` matching fails or the bounds fail the guard
`!(n == 0 || m == 0 || n > 32767 || m > 32767 || m <= n || trailing comma)`,
`re_compile_to()` emits `CHAR '\'` + `CHAR '{'` and keeps parsing. The pattern
then searches for the literal characters `\{...}`.

| pattern | Emacs meaning | tiny actually searches for | proof |
|---|---|---|---|
| `a\{0,3\}` | 0–3 × `a` | literal `a\{0,3}` | `a\{0,3}` in `za\{0,3}z` → `OK 1 7` |
| `a\{2,2\}` | exactly 2 | literal `a\{2,2}` | `xa\{2,2}x` → `OK 1 7` |
| `a\{1,1\}` | exactly 1 | literal | `a\{1,1\}`/`aaa` → NOMATCH |
| `a\{0\}` | empty match | literal `a\{0}` | `a\{0}` → `OK 0 5` |
| `a\{,0\}` | empty match | literal | NOMATCH vs `OK 0 0` |
| `a\{3,1\}` | **Emacs error** | literal `a\{3,1}` | `OK 0 7` |
| `a\{100000\}` | **Emacs error** | literal | NOMATCH |

`\{0,1\}` (a very natural "optional") and `\{n,n\}` are both in this set. The
user gets no error and a plausible-looking "not found", or worse a match on
text that happens to contain the literal braces. Highest-value D finding.

Reachable: **IS**, **QR**.

### D2. `\d`, `\D`, `\x41` are not Emacs syntax

Emacs treats `\d` as the literal `d` (a backslash before an ordinary character
is that character); it has no `\d`/`\D`/`\xNN` regexp escapes.

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `\d` | `5` | `OK 0 1` | NOMATCH |
| `\d` | `d` | NOMATCH | `OK 0 1` |
| `\D` | `x` | `OK 0 1` | NOMATCH |
| `\x41` | `A` | `OK 0 1` | NOMATCH |
| `\x41` | `x41` | NOMATCH | `OK 0 3` |

Both directions are silent. A user with Perl/PCRE habits gets what they expect
in kg but not in Emacs; a user with Emacs habits gets the opposite.

### D3. `\s` / `\S` swallow no argument, but Emacs' do

In Emacs `\sC` / `\SC` take a syntax-class character: `\s-` is whitespace,
`\sw` is word syntax, and a bare trailing `\s` is a *pattern error*.

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `\s-` | `  ` | NOMATCH (space then `-`) | `OK 0 1` |
| `\sw` | `ab` | NOMATCH (space then `w`) | `OK 0 1` |
| `\Sw` | `aw` | `OK 0 2` | NOMATCH |
| `\s` | `' '` | `OK 0 1` | **error** "Premature end of regular expression" |

This also means every Emacs pattern containing `\s.`/`\sw` silently re-scopes
the following character, which in a fuzz run repeatedly turned an Emacs-valid
pattern into an Emacs-*invalid* one (unbalanced `\(`) — i.e. the character
after `\s` is being eaten by one engine and not the other.

### D4. Word/symbol/buffer-boundary escapes silently become literal characters

Support for these is a declared non-goal; the finding is that they are *not*
rejected, they are quietly reinterpreted as the literal character after the
backslash.

| pattern | tiny reads as | Emacs | example |
|---|---|---|---|
| `\<a` | `<a` | word start | `\<a` / `a b` → NOMATCH vs `OK 0 1` |
| `a\>` | `a>` | word end | NOMATCH vs `OK 0 1` |
| `\ba` | `ba` | word boundary | NOMATCH vs `OK 0 1` |
| `\_<a` | `_<a` | symbol start | NOMATCH vs `OK 0 1` |
| `` \`a `` | `` `a `` | buffer start | NOMATCH vs `OK 0 1` |
| `a\'` | `a'` | buffer end | NOMATCH vs `OK 0 1` |

A pattern like `\bfoo\b` therefore silently searches for the literal string
`bfoob`, which *can* match real text.

### D5. `\1` is a literal `1`

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `\(a\)\1` | `aa` | NOMATCH | `OK 0 2` |
| `\(a\)\1` | `a1` | **`OK 0 2`** | NOMATCH |
| `a\1` | `aa` | NOMATCH | **error** "Invalid back reference" |

Backreferences are a declared non-goal, but the silent literal reinterpretation
means a backreference pattern can *match the wrong text* rather than fail.

### D6. Non-greedy quantifiers parse as "quantifier, then a node that never matches"

`a*?` compiles to `CHAR a`, `STAR`, `QUESTIONMARK`; the trailing quantifier node
falls through `matchone()`'s `default:` and compares against `u.ch == 0`, so the
pattern can never match anything at all.

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `a*?` | `aaa` | NOMATCH | `OK 0 0` |
| `a+?` | `aaa` | NOMATCH | `OK 0 1` |
| `a??` | `aaa` | NOMATCH | `OK 0 0` |
| `a*?b` | `aab` | NOMATCH | `OK 0 3` |
| `a\{2\}?` | `aaa` | NOMATCH | `OK 0 2` |

"Never matches anything" is at least loud in practice, but it is not an error
and the user gets `Regexp I-search` with no result and no explanation.

### D7. Shy groups `\(?:...\)` silently never match

`\(?:ab\)` / `ab` → tiny NOMATCH, Emacs `OK 0 2`. Emacs 31 supports
`\(?:...\)` and `\(?N:...\)`; kg's prompt accepts them without complaint.

### D8. Unknown POSIX class names degrade into a literal character set

`compile_charclass()` copies any `[:...:]` run verbatim; `matchnamedclass()`
recognises 11 names and anything else falls through to plain character
comparison against the copied bytes.

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `[[:blank:]]` | `a\tb` | **`OK 0 1`** (matched `a`!) | `OK 1 1` (the tab) |
| `[[:word:]]` | `a` | NOMATCH | `OK 0 1` |
| `[[:foo:]]` | `:` | `OK 0 1` | **error** "Invalid character class name" |
| `[[:foo:]]` | `abc` | NOMATCH | **error** |

`[[:blank:]]` matching the letter `a` (because `a` appears in the *spelling*
`[:blank:]`) is the purest example of "plausible-looking wrong result" in this
whole report. Supported names are digit, alpha, alnum, space, cntrl, graph,
print, punct, xdigit, lower, upper. Emacs also has blank, word, ascii,
nonascii, multibyte, unibyte.

### D9. Backslash inside `[...]` is an escape here, a literal member in Emacs

Emacs bracket expressions have no escape character: `[\d]` is the set
`{\, d}`.

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `[\]]` | `]` | `OK 0 1` | NOMATCH |
| `[a\]]` | `]` | `OK 0 1` | NOMATCH |
| `[\d]` | `5` | `OK 0 1` | NOMATCH |
| `[\d]` | `d` | NOMATCH | `OK 0 1` |
| `[\w]` | `_` | `OK 0 1` | NOMATCH |

`[\\]`, `[\n]`, `[\t]` happen to agree by coincidence.

### D10. Literal `^` and `$` in mid-pattern positions never match

Emacs makes `^` literal unless it is at the start of the pattern (or after
`\(` / `\|`), and `$` literal unless at the end (or before `\)` / `\|`).

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `a$b` | `a$b` | NOMATCH | `OK 0 3` |
| `a^b` | `a^b` | NOMATCH | `OK 0 3` |
| `^^` | `^` | NOMATCH | `OK 0 1` |
| `$$` | `$` | NOMATCH | `OK 0 1` |
| `^*` | `**` | NOMATCH | `OK 0 1` |

`^*` is the interesting one: tiny applies `STAR` to the `BEGIN` node (the
`j > 0` guard is satisfied), producing a pattern that can never match.

### D11. Unterminated `[` is accepted as a class

`[abc` / `abc` → tiny `OK 0 1`; Emacs errors "Unmatched [ or [^".
`[^abc` / `xyz` → tiny `OK 0 1`; Emacs errors.
`[[:digit:` / `abc` → tiny NOMATCH (no error); Emacs errors.
(`[^` alone *is* rejected — the guard only covers the empty inverted class.)

While typing `[a-z` incrementally, kg shows results for a pattern Emacs would
call invalid.

### D12. Leading `*`, `+`, `?` are rejected, but Emacs treats them as literals

`*`, `+`, `?`, `**` at position 0 → `RE_STATUS_BAD_PATTERN`, so kg says
"Invalid regular expression"; Emacs matches them literally
(`(string-match "+" "a+b")` → 1). The converse direction of D1–D11: kg rejects
something Emacs accepts. Low impact, mentioned for completeness.

### D13. `RE_STATUS_TOO_COMPLEX` is reported to the user as "no match"

`kg_regex_match_forward()` maps every non-`RE_STATUS_OK` status to
`KG_REGEX_NOMATCH` (`src/regex.c:56`). A pattern that exhausts
`MAX_MATCH_STEPS` (e.g. `a.*.*.*.*.*.*b` on a 60-byte non-matching row) is
indistinguishable from a genuine miss. I did not find a case where the budget
produced a false negative on a subject that *does* match (all such probes
succeeded well inside the budget), so today this is a reporting issue, not a
correctness one — but it will silently become a correctness issue if the budget
is ever tightened.

---

## E — byte vs character semantics

kg now has full UTF-8 display-width handling; the regex engine is the only
byte-oriented layer left. All of these are **QR**-destructive.

### E1. `.` matches one byte of a multi-byte glyph — proven file corruption

`.` / `åbc`: tiny `OK 0 1` (the lead byte `0xC3`), Emacs `OK 0 2` (bytes) / one
character.

End to end (`utf8.yaml`, real `src/kg`): line `åbc`,
`M-x query-replace-regexp` `.` → `X`, answer `y` then `q`:

```
--- expected     Xbc
+++ actual       X<0xa5>bc
```

kg wrote a stray UTF-8 continuation byte into the user's file. The file is now
invalid UTF-8.

### E2. Negated classes and multi-character classes match single bytes

* `[^x]` / `å` → `OK 0 1` (half a glyph).
* `[åä]` / `ä` → `OK 0 1` — it matched the **shared lead byte `0xC3`**, so
  `[åä]` matches the first byte of *any* Latin-1-supplement character.
* `[å-ö]` / `æ` → `OK 0 1`, again the lead byte; the "range" is a byte range.

### E3. Quantifiers count bytes, so they attach to the last byte of a glyph

| pattern | subject | tiny | Emacs |
|---|---|---|---|
| `å+` | `åå` | `OK 0 2` (one `å`) | `OK 0 4` (two) |
| `å*` | `ååå` | `OK 0 2` | `OK 0 6` |
| `.\{2\}` | `åä` | `OK 0 2` (one glyph) | `OK 0 4` (two) |

`å+` compiles to `CHAR 0xC3`, `CHAR 0xA5`, `PLUS` — the `+` applies to `0xA5`
alone. This is not just a length difference: `å\{2\}` means "one `å`" here.

### E4. `.` cannot span a multi-byte character

`a.c` / `aåc` → NOMATCH (Emacs `OK`); `x.y` / `x中y` → NOMATCH (Emacs `OK`).

### E5. Case folding is ASCII-only

`Å` vs `å` and `å` vs `Å` with `icase=1` → NOMATCH; Emacs matches. kg turns
ICASE on automatically for any lowercase-only query, so this bites by default.
Note `query_has_upper()` also cannot see `Å` as uppercase.

### E6. `\w` and `[[:alpha:]]` are `<ctype.h>`/C-locale

`\w` / `å` → NOMATCH; `[[:alpha:]]` / `å` → NOMATCH. Emacs matches both.
Word-ish classes over non-ASCII text are simply unusable.

### E7. Whole-glyph literals do work

`å` / `å`, `中` / `a中b`, `.*` over `åäö`, `[^å]` / `å` all agree — plain literal
UTF-8 search is fine. Only classes, `.`, quantifiers and case folding break.

---

## C — unsupported, fails cleanly (low value, noted only)

1. **More than 9 capture groups** → `RE_STATUS_BAD_PATTERN` (`num_groups >=
   RE_MAX_SPANS`). `\(a\)` × 10 is rejected; Emacs allows far more. Clean
   rejection, but the limit is low and the message is just "Invalid regular
   expression".
2. **Unbalanced `\(` / `\)`** → `BADPAT`; Emacs errors too. Agrees.
3. **Trailing lone `\`** → `BADPAT`; Emacs errors. Agrees.
4. **Multi-line `^`/`$`** (`a$` on `"a\nb"`) differ, but multi-line matching is
   a declared non-goal and kg matches row-locally, so a row never contains a
   newline. Not a real exposure.
5. **`.` and `\r`**: `matchdot()` excludes both `\n` and `\r`; Emacs `.`
   excludes only `\n`. Only observable on a row that retains a CR (CRLF file);
   `[^x]` does match `\r` in both.

---

## Probed and found NO difference

Negative results, so you know where not to look next. All verified against the
Emacs oracle at byte-offset granularity.

* **Literals and basic structure**: literal strings, `.`, `^`/`$` at the true
  pattern edges, `^$`, `x*$`, `$` at end of string, `^` on non-empty text.
* **Greedy `*` and `+` on plain atoms, including backtracking**: `a*a`, `a+a`,
  `.*c`, `[0-9]*5`, `a+bc+`, `a+.+c+`, `foo.*`, `^.*$`, `^\(.*\)$`.
* **Leftmost-first alternation preference** (where alternation works at all):
  `a\|ab` on `ab` → both prefer the first alternative (`OK 0 1`); `ab\|a` on
  `ab` → both `OK 0 2`. tiny is leftmost-first, like Emacs; I found no
  leftmost-*longest* behaviour.
* **Character classes**: `[abc]`, `[^abc]`, `[a-c]`, `[a-]`, `[-a]`, `[0-9-]`,
  `[+--]`, `[c-a]` (empty range → no match in both), `[.]`, `[*]`, `[$^]`,
  `[[]`, `[\\]`.
* **The 11 supported POSIX classes**: digit, alpha, alnum, space, cntrl, graph,
  print, punct, xdigit, lower, upper — including `[^[:digit:]]` and
  `[[:digit:]abc]` mixtures, and `[:digit:]` (no outer brackets) as a plain
  character set. `[[:ascii:]]` and `[[:multibyte:]]` agree on ASCII input by
  coincidence.
* **ASCII case folding**: literals, `[a-z]`/`[A-Z]` ranges, `[abc]`,
  `[^a-z]`, `[^A-Z]`, `[[:upper:]]`/`[[:lower:]]`/`[[:alpha:]]` under ICASE, and
  `\w` — all agree with Emacs `case-fold-search`. kg's smart-case heuristic
  (`fold = !query_has_upper(query)`) matches Emacs isearch's own
  upper-case-disables-folding rule for ASCII.
* **Zero-length matches**: `a*` on `bbb`, `x*` on `abc`, `\(\)` on `abc` and on
  `""`, `\(a*\)` on `bbb`, `""` on `abc` — all `OK 0 0` in both, with matching
  capture spans. D5 of the roadmap looks fine at the engine level.
* **Capture spans** for ordinary patterns: sequential `\(a\)\(b\)`, nested
  `\(a\(b\)\)`, repeated `\(a\)*` (last iteration wins, same as Emacs),
  quantified `\(ab\)\{2\}`, `\(a\|b\)*`, `\(ab*\)*`, unmatched optional group
  (`b\(a\)*c` → tiny `-1,-1`, Emacs `nil` — same meaning),
  `\(\w+\)=\(\w+\)`, `\([a-z]+\) \([a-z]+\)`.
* **Quantified groups that *do* work**: `\(ab\)*ab`, `\(ab\)+ab`,
  `\(ab\)\{2\}c`, `x\(a\|b\)\{2\}y`, `\(.\)+c` — `matchgrouptimes()` backtracks
  correctly across the group boundary, unlike A3 which is inside it.
* **Interval forms that do work**: `\{n\}`, `\{n,\}`, `\{,m\}`, `\{n,m\}` with
  `0 < n < m` — `a\{2\}`, `a\{,3\}`, `a\{3,\}`, `a\{1,3\}`, `[ab]\{2\}`,
  `.\{3\}`, `\w\{2\}`, `\(ab\)\{1,2\}`.
* **Bare `(`, `)`, `|`, `{`, `}`** are literal in both (`(ab)`, `a|b`, `a{2}`).
  The Emacs-shaped dialect decision holds.
* **Unknown escapes of ordinary characters**: `\.`, `\\`, `\-`, `\n` (→ `n`),
  `\t` (→ `t`), `a\z` (→ `az`) — all agree.
* **Whole-glyph UTF-8 literal search** (E7).
* **ReDoS / hangs**: none found; step budget holds runtime to ~0.02 s on every
  catastrophic-backtracking shape I tried.
* **Engine memory safety**: 22 000 sanitizer runs across malformed and
  oversized patterns produced no ASan/UBSan report.
* A 250-case structured fuzz over `.`/classes/`*`/`+`/groups (no `?`, no `\|`,
  no intervals) produced **0** diffs — the healthy core of the dialect is
  genuinely healthy.

---

## Ambiguous cases I am deliberately not adjudicating

* **A4 (`?` non-greedy)** — `re.h` documents it, so it is intentional, but it
  contradicts the Emacs dialect goal. Which document wins is your call.
* **D12** — Emacs' "leading `*` is literal" is arguably a wart worth not
  copying; rejecting is defensible.
* **A8** — silent truncation vs `RE_STATUS_BUFFER_TOO_SMALL` is a policy choice;
  it is currently unreachable from kg's UI either way.
* **D13** — whether `TOO_COMPLEX` should surface to the user as a distinct
  message is a UX decision, not a correctness one, at today's budget.
