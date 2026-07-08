## Master plan: Emacs-like regular expressions across tiny-regex-c, fe, and kg

### Goal

Add regular-expression support in three coordinated layers:

1. `tiny-regex-c`: the shared regex engine and dialect.
2. `fe`: Lisp-facing regex objects and match functions.
3. `kg`: editor commands such as `isearch-forward-regexp`, `isearch-backward-regexp`, and eventually `query-replace-regexp`.

The intended end state is:

* `kg` editor regex search works even when built with `WITH_LISP=0`.
* `fe` Lisp has regex capabilities through its native extension layer.
* Both `kg` and `fe` use the same underlying `tiny-regex-c` engine.
* The regex dialect is made more Emacs-like inside `tiny-regex-c` itself; avoid kg-side or fe-side dialect adapters.
* Capture spans are supported before regexp replacement is implemented.

---

## Design decisions

### D1. Regex belongs in both fe and kg

Regex is both:

* a Lisp language capability for `fe`;
* an editor primitive for `kg`.

Do not make kg’s editor search call into Fe Lisp evaluation. kg should use a small C regex wrapper directly.

### D2. kg regex must not depend on `WITH_LISP`

`isearch-forward-regexp`, `isearch-backward-regexp`, and `query-replace-regexp` should be available in kg even when Lisp support is disabled.

Lisp bindings may disappear under `WITH_LISP=0`, but editor regex should remain available.

### D3. No dialect adapters

The regex dialect should be reworked in `tiny-regex-c` itself to become more Emacs-like.

Avoid translating Emacs-ish patterns in kg or fe before passing them to the engine.

### D4. Capture spans are API-shaping work

Capture spans should be designed early because they affect:

* C result structures;
* Fe result shape;
* replacement expansion;
* tests;
* future scripting compatibility.

However, full capture implementation does not need to block the first working regexp isearch. A whole-match-only implementation can use the final capture-ready result shape with `nspans == 1`.

### D5. Zero-length matches must always advance

All iterative search APIs must define safe zero-length behavior.

Recommended rule:

* forward iteration: after a zero-length match, advance the candidate start by one byte;
* backward iteration: search over strictly earlier candidate positions so the same zero-length match is not rediscovered forever;
* replacement loops: after replacing a zero-length match, advance past the insertion point by at least one byte, or stop at end of row/buffer.

For the initial kg implementation, keep regexp matching row-local. Multi-line regexp matching can be a later project.

---

## Execution order

### Phase 0 — Synchronize branches and submodules

Repositories and branches:

* `kg`: branch `stricter-emacs-adherence`
* `fe`: branch `analyzers-etc`
* `tiny-regex-c`: branch `adapt-to-fe`

Tasks:

1. Confirm kg’s `fe` submodule points to the intended `fe` commit.
2. Confirm fe’s `tiny-regex-c` submodule points to the intended regex-engine commit.
3. Add/update `.meta-docs/plans/` documents in all three repositories.
4. Do not move `tiny-regex-c` out of `fe` unless a later build-system problem forces it.

Rationale:

`fe` is the natural owner of the regex engine dependency because regex is becoming an `fe` capability, while kg can still compile the nested engine from `fe/tiny-regex-c`.

---

### Phase 1 — tiny-regex-c: capture-ready API and match result model

Primary plan document:

* `tiny-regex-c/.meta-docs/plans/001-EMACS-LIKE-REGEX-ENGINE.md`

Objectives:

1. Add a capture-ready match-result API.
2. Preserve or wrap existing APIs during transition.
3. Add tests for whole-match spans, no-match, invalid-pattern, and zero-length matches.
4. Ensure callers can use caller-owned compiled regex storage instead of the static `re_compile()` buffer.

Deliverables:

* `re_span` or equivalent start/end span structure.
* `re_match_result` or equivalent result structure.
* A status/error enum.
* Compile API that reports invalid patterns distinctly from no-match.
* Match API that returns at least whole-match span as span 0.
* Unit tests.

This phase may initially report only the whole match. Capture groups can remain unimplemented as long as the public result shape is capture-ready.

---

### Phase 2 — tiny-regex-c: Emacs-like dialect work

Primary plan document:

* `tiny-regex-c/.meta-docs/plans/001-EMACS-LIKE-REGEX-ENGINE.md`

Objectives:

1. Rework parsing to support the chosen Emacs-like dialect directly.
2. Avoid kg-side or fe-side pattern adapters.
3. Decide and document the exact supported subset.

Suggested MVP dialect:

* `.` matches any byte except newline.
* `^` and `$` anchor to row/string boundaries.
* Character classes: `[abc]`, `[^abc]`, ranges.
* POSIX bracket classes where practical: `[[:digit:]]`, `[[:alpha:]]`, etc.
* Repetition: `*`, `+`, `?`.
* Emacs-style grouping: `\(...\)`.
* Emacs-style alternation: `\|`.
* Emacs-style intervals: `\{n\}`, `\{n,\}`, `\{n,m\}`.
* Word-ish escapes only after explicit decision; do not accidentally promise full Emacs syntax.

Out of scope for MVP:

* multi-line regex;
* full Unicode character classes;
* syntax-table-aware Emacs constructs;
* replacement expansion;
* backreferences in the matcher.

---

### Phase 3 — fe: replace POSIX regex extension with tiny-regex-c-backed extension

Primary plan document:

* `fe/.meta-docs/plans/101-ADD-REGEX.md`

Objectives:

1. Keep regex as an Fe extension, not part of the tiny Fe core.
2. Replace the current POSIX `<regex.h>` backend with `tiny-regex-c`.
3. Expose stable Lisp-facing functions whose result shape can represent captures.
4. Add tests in fe for compile, match, invalid pattern, no-match, and capture-ready result shape.

Recommended Lisp API:

```lisp
(compile-re PATTERN)          ; -> regex object or error
(match-re REGEX TEXT)         ; -> nil or list of spans
(re-match PATTERN TEXT)       ; optional one-shot helper
(re-match? PATTERN TEXT)      ; optional predicate helper
```

Recommended match result:

```lisp
((START END) (CAP1-START CAP1-END) ...)
```

Where:

* span 0 is the whole match;
* later spans are captures when implemented;
* missing captures can be represented as `nil`;
* indexes are byte offsets for now.

If compatibility with existing `match-re` behavior is important, document the intentional break or add a temporary compatibility helper.

---

### Phase 4 — kg: build and C wrapper integration

Primary plan document:

* `kg/.meta-docs/plans/102-REGEX-EDITOR-INTEGRATION.md`

Objectives:

1. Compile `fe/tiny-regex-c/re.c` into kg independently of `WITH_LISP`.
2. Add `src/regex.c` / `src/regex.h` as kg’s wrapper around the engine.
3. Ensure the wrapper owns kg-specific semantics:

   * smart-case flags;
   * row-local matching;
   * forward/backward search helpers;
   * zero-length match advancement;
   * status messages for invalid regexes.
4. Do not put editor search through Lisp evaluation.

Deliverables:

* Makefile additions for `REGEX_OBJ`.
* `kg_regex_compile`, `kg_regex_match_forward`, `kg_regex_match_backward`, or equivalent.
* Unit tests for the wrapper.
* No editor UI changes yet, unless convenient.

---

### Phase 5 — kg: regexp isearch commands

Primary plan document:

* `kg/.meta-docs/plans/102-REGEX-EDITOR-INTEGRATION.md`

Objectives:

1. Add named commands:

   * `isearch-forward-regexp`
   * `isearch-backward-regexp`
2. Reuse the existing incremental-search UI as much as possible.
3. Preserve smart-case behavior.
4. Highlight whole-match spans.
5. Handle invalid regex while typing without crashing or exiting search.

Implementation recommendation:

Refactor current literal isearch into a mode-driven implementation:

```c
enum search_kind {
    SEARCH_LITERAL,
    SEARCH_REGEXP,
};
```

Then share:

* prompt handling;
* direction handling;
* handoff keys;
* highlight restore;
* screen reveal.

Only the actual “find match” primitive should differ.

Initial keybindings can be deferred. M-x command availability is enough for the first phase.

---

### Phase 6 — tiny-regex-c: capture spans

Primary plan document:

* `tiny-regex-c/.meta-docs/plans/001-EMACS-LIKE-REGEX-ENGINE.md`

Objectives:

1. Implement capture-span recording for `\(...\)` groups.
2. Preserve whole-match span as span 0.
3. Define behavior for:

   * unmatched optional captures;
   * repeated captures;
   * nested captures;
   * capture-count limits.
4. Add tests before exposing replacement expansion in kg.

Recommended behavior:

* span 0: whole match;
* span N: Nth capturing group in left-parenthesis order;
* unmatched optional group: invalid span, e.g. `start = -1`, `end = -1`;
* repeated group: last successful capture wins, unless a better rule is explicitly chosen;
* exceeding capture limit: compile error or explicit “too many captures” status.

---

### Phase 7 — kg: query-replace-regexp

Primary plan document:

* `kg/.meta-docs/plans/102-REGEX-EDITOR-INTEGRATION.md`

Objectives:

1. Add named command:

   * `query-replace-regexp`
2. Preserve existing query-replace interaction:

   * prompt for pattern;
   * prompt for replacement;
   * `y`, `n`, `!`, `q`, `C-g` behavior;
   * one undo record per accepted replacement.
3. Add replacement expansion after capture spans are available.

Recommended replacement syntax:

* `\&` means whole match;
* `\1` through `\9` mean capture groups;
* `\\` means literal backslash;
* unknown escapes should either remain literal or be rejected; choose and test.

Initial implementation may support only row-local matches.

---

### Phase 8 — polish, docs, and keybindings

Objectives:

1. Document supported regex syntax in:

   * `tiny-regex-c`;
   * `fe`;
   * `kg README.md`;
   * `kg doc/kg.1`.
2. Add M-x command docs.
3. Add keybindings only after command behavior is stable.

Potential keybindings:

* `C-M-s` or `ESC C-s` → `isearch-forward-regexp`
* `C-M-r` or `ESC C-r` → `isearch-backward-regexp`
* `C-M-%` or `ESC C-%` → `query-replace-regexp`

Terminal encodings for control-meta keys vary, so treat keybindings as a later compatibility task.

---

## Dependency graph

```text
tiny-regex-c Phase 1
    |
    +--> tiny-regex-c Phase 2
            |
            +--> kg Phase 4
            |       |
            |       +--> kg Phase 5
            |
            +--> fe Phase 3
            |
            +--> tiny-regex-c Phase 6
                    |
                    +--> kg Phase 7
```

---

## Non-goals for the first pass

* Full Emacs regex compatibility.
* Multi-line regexp search.
* Unicode-aware character classes.
* Backreferences in the matcher.
* Syntax-table-aware word/symbol classes.
* Replacing kg search by Lisp evaluation.
* Moving `tiny-regex-c` out of the `fe` submodule tree.


## ADDENDUM TO THIS MASTER PLAN

An external reviewer of this plan, and its subplans made the following verdict:
<external_reviewer_verdict>
4. Add explicit compile-size / storage-resize semantics to tiny-regex-c
The tiny plan introduces re_compile_checked(pattern, storage, storage_size, out) and RE_STATUS_BUFFER_TOO_SMALL, which is the right direction.
But it should say what happens to *storage_size on failure:

Does it return required size?

Does it return bytes consumed before failure?

Should callers use a fixed RE_MAX_COMPILED_BYTES instead?

Can Fe retry with a larger allocation?

I’d add a small subsection:

```
On RE_STATUS_BUFFER_TOO_SMALL, *storage_size is updated to the required size when knowable. If the required size is not knowable without a second parse, expose a re_compiled_size(pattern, flags, &size) helper or define a conservative RE_MAX_COMPILED_BYTES.
```

This matters for Fe because the plan says the compiled regex object should own heap storage.
5. Decide whether status lives in return value or result struct
The tiny plan currently has both:

```C
re_status re_exec(..., re_match_result *out);
```

and:

```C
typedef struct {
    re_status status;
    ...
} re_match_result;
```

That duplicates status state.
I’d recommend: return re_status; keep re_match_result only for spans. If you keep both, explicitly say they must match.
6. Add flags to the initial checked compile API
The plan introduces RE_FLAG_ICASE later in Phase 4.
Since kg smart-case needs this and Fe tests mention case-insensitive matching later, I’d make the first new API flag-ready:

```C
re_status re_compile_checked(
    const char *pattern,
    re_flags flags,
    unsigned char *storage,
    unsigned *storage_size,
    re_t *out);
```

That avoids immediately revising the API after Phase 1.
7. Add Fe build-system details
The Fe plan correctly says to replace <regex.h> with tiny-regex-c.
But it should explicitly include Makefile tasks:

compile tiny-regex-c/re.c;

add include path -Itiny-regex-c;

link regex object into fe, tests, example host, fuzz targets if needed;

clean regex object;

ensure submodule-init failure has a useful message.

Right now the Fe plan focuses on object model and Lisp API, but not the concrete build integration.
8. Add “no fixed Fe string buffers” to Fe plan
The old plan had an important concern about avoiding fixed stack buffers. The new Fe plan should explicitly say: use FeStringByteLength + FeCopyStringBytes or equivalent malloc-backed copying for patterns/text, not fixed FeToString buffers.
That matters because regex over editor/user strings should not silently truncate.
kg-specific improvements
9. Add nested-submodule checks for WITH_LISP=0
The kg plan says regex is independent of WITH_LISP and that kg will still need the fe submodule solely for tiny-regex-c when WITH_LISP=0.
Add an explicit build check like:

```
Makefileifeq ($(wildcard fe/tiny-regex-c/re.c),)
$(error fe/tiny-regex-c/re.c is missing; run 'git submodule update --init --recursive')
endif
```

This avoids a confusing “No rule to make target” failure.
10. Split engine object and wrapper object names
The kg plan uses REGEX_OBJ = $(OBJDIR)/re.o, but also plans src/regex.c / src/regex.h.
I’d name them separately:

```
MakefileREGEX_ENGINE_OBJ = $(OBJDIR)/tiny_regex.o
REGEX_WRAPPER_OBJ = $(OBJDIR)/regex.o
REGEX_OBJS = $(REGEX_ENGINE_OBJ) $(REGEX_WRAPPER_OBJ)
```

This avoids confusion between re.o from the submodule and kg’s wrapper.
11. Clarify row render vs chars
Current kg literal isearch and query replace are not identical domains: isearch is visual/render-oriented, while replacement must operate on actual row bytes. The kg plan says spans are byte offsets into the row string, but it should explicitly say which row string:

isearch may search row->render, requiring highlight/cursor mapping;

query replace must search row->chars, because edits apply to real buffer text.

The plan already calls out row-local matching and highlighting, but this render/chars distinction is worth making explicit before implementation.
12. Make backward-match semantics exact
The kg plan says backward search should “find last match whose start/end is before or at the requested boundary.”
I’d specify:

```
Backward search returns the last match with match.end <= before. For zero-length matches, require match.start < before after the first visit to avoid rediscovery.
```

That matches the existing literal backward-search spirit better.
Verdict
The plans are in order conceptually. I would not rework the overall direction. The important changes are:

move the master roadmap under kg/.meta-docs/plans/;

fix escaped C identifiers in kg markdown;

make kg command exposure depend on the Emacs-like dialect phase;

tighten tiny-regex-c API details around flags, status, and compile-storage sizing;

add concrete Fe build steps and no-fixed-buffer guidance.

After those, I’d consider the plan set ready to implement.

</external_reviewer_verdict>
Make of this what you like.
