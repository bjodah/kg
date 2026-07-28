# Recommended regex fixes, triaged

Triage of a differential-testing expedition against `fe/tiny-regex-c/`,
using GNU Emacs 31 as the oracle. Raw findings, including the ones judged
not worth acting on, are in `.meta-docs/ideation/regex-emacs-discrepancies.md`.

Scope note: `100-REGEX-MASTER-ROADMAP.md` already declares backreferences,
syntax-table classes, Unicode character classes and multi-line matching to
be non-goals. Nothing below asks for those. Everything here is either a
memory-safety defect, or a construct the engine **claims to support** and
gets wrong, or a construct it silently reinterprets instead of rejecting.

Findings marked *(verified independently)* were reproduced from scratch
against a separately written driver and a fresh Emacs oracle, not taken on
report. The rest are as reported.

## Priority order

| # | Item | Class | User impact |
|---|---|---|---|
| P0-1 | Match span can end past end of subject | memory safety | heap over-read; file corruption |
| P0-2 | Spans are used unvalidated by the wrapper | missing defence | turns any engine bug into a buffer overflow |
| P1-1 | `\|` binds one atom on the left, everything on the right | wrong answer | alternation is unusable |
| P1-2 | Backtracking holes in groups and intervals | wrong answer | canonical patterns silently fail |
| P1-3 | Byte-oriented matching corrupts UTF-8 | wrong answer | writes invalid bytes into user files |
| P2-1 | Invalid/unsupported intervals degrade to literal text | silent misparse | plausible-looking "not found" |
| P2-2 | Unknown POSIX class names degrade to a literal set | silent misparse | `[[:blank:]]` matches `a` |
| P2-3 | `^` anchors at `start_offset`, not at line start | wrong answer | `^` matches mid-line |
| D-1 | `?` is non-greedy by design | decision needed | contradicts the Emacs dialect goal |

---

## P0-1 — `re_exec` reports `end` past the end of the subject

*(verified independently)*

```
pattern            subject   reported span   strlen
a*.c+              ac        [0,3)           2
\(.*.a\{2\}\)      baa       [0,5)           3
```

Observed on 0.37% of patterns randomly generated **from the documented
grammar**, with overshoot up to +22 bytes. Both example patterns are
ordinary; neither is malformed.

Consequences, both in shipped code:

* ASan heap-buffer-overflow READ via `src/search.c:845`, which hands
  `match_len` to `undo_push`, which `memcpy`s that many bytes out of
  `row->chars` — a `malloc(size + 1)` allocation.
* `query-replace-regexp` of `\w*.\{3\}a?[a-c]\{1\}` → `X` on the line
  `b11bZZZ` deletes the entire line where Emacs produces `XZZZ`. It
  destroys text that was never part of any match.

**Fix the engine.** This is the one item that is a plain defect rather
than a dialect question: a match cannot legitimately end past the subject.
Fix before anything else here, because the parser changes proposed below
will move the bug around rather than remove it.

## P0-2 — The wrapper trusts the engine's span

*(verified independently)*

`kg_regex_match_forward()` in `src/regex.c` calls `re_exec()` and, on
`RE_STATUS_OK`, copies the result straight out with no comparison against
`strlen(text)`. That is what converts P0-1 from a wrong number into a heap
over-read and a corrupted file.

**Add a validation layer in the wrapper, independent of fixing P0-1.**
Reject or clamp any span with `start < 0`, `end > len`, or `end < start`.
Defence in depth is warranted here specifically because the engine is
vendored, is being actively modified, and is reachable from an interactive
command that rewrites the user's buffer. A future engine change should not
be able to corrupt a file again.

Cheap, self-contained, and it can land before the engine fix.

---

## P1-1 — Alternation binds the wrong operands

*(verified independently)*

`\|` takes only the single preceding **atom** on the left, and the whole
remainder on the right, so `ab\|cd` parses as `a\(b\|cd\)`.

```
pattern      subject   kg        emacs
foo\|bar     bar       NOMATCH   [0,3)
ab\|cd       cd        NOMATCH   —
za\|b        zb        [0,2)     [1,2)
```

The last row is the dangerous shape: both engines match, at different
offsets and lengths, so nothing looks wrong.

Alternation is one of the headline reasons to offer an Emacs dialect at
all, and in this state `foo\|bar` — the form every user writes first —
cannot match its own right-hand side. Fix the parser so `\|` separates
concatenations, not atoms.

## P1-2 — Backtracking holes in groups and intervals

*(verified independently)*

```
pattern              subject   kg        emacs
^\(.*\),\(.*\)$      a,b       NOMATCH   [0,3)
.\{2,3\}c            abc       NOMATCH   [0,3)
```

`*` and `+` on plain atoms backtrack correctly; groups and intervals do
not give back input once they have consumed it. The first pattern is the
canonical field-swap `query-replace-regexp`, so this is not an exotic
corner.

## P1-3 — Byte-oriented matching corrupts UTF-8

*(verified independently)*

The engine matches bytes; Emacs matches characters. The reported offsets
can look identical while meaning different things:

```
pattern    subject   kg                        emacs
.\{2\}     åb        [0,2) = both bytes of å   [0,2) = å and b
```

`query-replace-regexp` of `.` → `X` on `åbc` writes a stray `0xA5`
continuation byte into the saved file. `[åä]` matches the shared `0xC3`
lead byte of any Latin-1-supplement character.

Making the engine character-aware is a large change and arguably a
non-goal. **The corruption is separately preventable**: have the wrapper
snap match boundaries to UTF-8 glyph boundaries before returning them. kg
already has `utf8_glyph_span_at()` and, since the display-width work,
`src/width.c`. That keeps invalid bytes out of user files without
touching the engine, and it composes with the P0-2 validation layer —
same place, same pass.

Worth doing regardless of whether full character-awareness is ever
attempted, and it is now the only part of kg that still reasons in bytes
where the rest reasons in glyphs.

---

## P2-1 — Invalid intervals become literal text

Any `\{n,m\}` with `n == 0`, `m <= n`, or a very large count falls back to
matching the literal characters. `x\{0,1\}` searches for the seven-character
string `x\{0,1}` *(verified independently: NOMATCH against `x`, matches
against the literal spelling)*.

No error is raised, so the user gets a plausible "not found". **Make these
a compile error.** A rejected pattern is recoverable; a silently
reinterpreted one is not.

## P2-2 — Unknown POSIX class names become a literal set

`[[:blank:]]` matches the letter `a`, because an unrecognised class name
degrades to a set of the characters in its own spelling. Emacs raises an
error. Same fix and same reasoning as P2-1.

Note the 11 classes that *are* supported were checked and agree with Emacs
exactly, so this is only about the unrecognised ones.

## P2-3 — `^` anchors at `start_offset`

`^` anchors wherever `re_exec` was told to start, and kg passes the cursor
column, or the end of the previous match. So `^a` matches mid-line. This
is a wrapper-level bug, not an engine one: the engine needs to be told
where the line actually begins, separately from where the scan resumes.

---

## D-1 — Decision needed: `?` is non-greedy

`re.h` documents `?` as non-greedy, deliberately. Emacs' `?` is greedy.
This is a genuine dialect choice rather than a defect, and the expedition
correctly declined to pick a side.

Recommendation: make it greedy, matching Emacs. The whole point of the
backslashed dialect is that patterns written for Emacs behave the same
here, and a silently non-greedy `?` violates that with no error. If it
stays non-greedy, it needs to be prominent in `doc/kg.1`, not just in a
vendored header.

Three further items were flagged ambiguous in the raw findings and are
left open there.

---

## Where not to look

The expedition found **no** divergence in: greedy `*` and `+` on plain
atoms, character classes and ranges, the 11 supported POSIX classes, ASCII
case folding, zero-length match handling, ordinary capture spans,
leftmost-first preference, and bare `(`, `|`, `{` treated as literals. A
250-case fuzz over that subset produced zero differences.

No hangs and no ReDoS: the step budget holds every probe to ~0.02s.
**Zero** ASan/UBSan reports across 22,000 malformed and oversized-pattern
runs — the engine's internal bounds handling is sound. P0-1 is not a
memory bug inside the engine; it is a correct-memory engine reporting an
incorrect number, which the caller then trusts.

## Suggested sequencing

1. **P0-2** first, alone. It is small, it is independent, and it stops the
   file corruption immediately even though the engine is still wrong.
2. **P0-1**, with the P0-2 assertion left in as a regression net.
3. **P1-3**'s boundary snapping, in the same wrapper pass as P0-2.
4. **P2-1** and **P2-2** together — both are "reject instead of
   reinterpret", both are small, and both convert silent wrong answers
   into errors.
5. **P1-1**, then **P1-2**. Parser work; do it after the safety net exists.
6. **P2-3** and **D-1** at leisure.

Institutionalise the differential fuzzer rather than discarding it. It
found P0-1 at a 0.37% hit rate, which no hand-written case would have
reached. `test/fuzz_regex.c` already exists as a home for a corpus, and
every pattern above belongs in it as a regression case.
