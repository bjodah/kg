# Sub-plan 06-F — Include hygiene: make ci-06 green

Closes CI lane **ci-06 (static analysis / IWYU)**, red since before Plan
06 began.

Like sub-plan E, this is debt rather than one of Plan 06's phases.  It is
mechanical, but it is not blind: three of the findings must be refused or
adapted rather than applied, and the ones that touch headers interact with
`make header-check` and the `WITH_LISP=0` configuration.

---

## Problem

`.ci/ci-06-static-analysis.sh` runs `iwyu_tool.py` with `-Xiwyu
--error=1`, so any finding fails the lane.  There are findings in **16
files**.  The oldest predate Plan 06 entirely — `src/register.[ch]` is
Plan 05's, `src/compile_nav.[ch]` and `src/compile_parse.h` are older
still — which is why the lane was already red at `0123432` with the same
32 finding-lines it has now.

Sub-plan D fixed only the files its own slices created
(`src/lisp_process.c`, `src/lisp_require.c`) and removed three redundant
`<stddef.h>` includes, on the principle that a slice cleans up after
itself.  This sub-plan does the rest.

---

## The findings

Regenerate with `.ci/ci-06-static-analysis.sh` (or `make iwyu`); this is
the state as of `96c408e`.

| File | Add | Remove / forward-declare |
|------|-----|--------------------------|
| `src/compile_nav.c` | `bufhandle.h`, `cmd.h`, `compile_parse.h`, `<limits.h>` | — |
| `src/compile_nav.h` | `<stddef.h>`; fwd-decl `struct compile_diag`, `struct kg_buffer_handle` | `bufhandle.h`, `compile_parse.h`, `<stdbool.h>` |
| `src/compile_parse.h` | — | `<stdbool.h>` |
| `src/lisp_buffer.c` | `bufhandle.h`, `lisp_obj.h`, `localvars.h` | `<stddef.h>` |
| `src/lisp_cmd.c` | `keyevent.h`, `lisp_obj.h` | `<stddef.h>` |
| `src/lisp_core.c` | `lisp_obj.h` | `<stdio.h>`, `<string.h>`, **`lisp.h` — see below** |
| `src/lisp_hooks.c` | `bufhandle.h`, `lisp_obj.h`, `<setjmp.h>`, `<stdint.h>` | `<stddef.h>` |
| `src/lisp_io.c` | `bufhandle.h`, `lisp_obj.h` | `<stddef.h>` |
| `src/lisp_obj.c` | `<stdlib.h>` | `edit.h`, `syntax.h` |
| `src/lisp_obj.h` | — | `<stdbool.h>`, `<stdint.h>` |
| `src/lisp_search.c` | `bufhandle.h`, `localvars.h`, `marker.h` | `<stddef.h>`, `<stdlib.h>` |
| `src/lisp_word.c` | `localvars.h`, `marker.h` | `<stddef.h>` |
| `src/process_table.c` | `process.h`, `<sys/types.h>` | — |
| `src/process_table.h` | fwd-decl `struct kg_spawn_request` | `process.h` |
| `src/register.c` | `bufhandle.h`, `keyevent.h` | — |
| `src/register.h` | fwd-decl `struct kg_buffer_handle` | `bufhandle.h`, `<stdbool.h>` |

`<stdbool.h>` removals are C23 doing its job — `bool` is a keyword — and
`CLAUDE.md` already names this as something IWYU is right about.

---

## One finding to refuse, one thing to check

**1. `src/lisp_core.c` must keep `#include "lisp.h"`.**  IWYU is
reasoning about symbol *use*, and `lisp_core.c` uses nothing *from*
`lisp.h` — it *implements* it.  Dropping the include removes the only
thing that checks the definitions against the declarations, so a signature
could drift silently and `WITH_LISP=0`'s stubs would be the first to
notice.  Suppress rather than obey:

```c
#include "lisp.h"  // IWYU pragma: keep
```

There is no existing `IWYU pragma` in `src/`, so this establishes the
convention.  Comment *why* at the site, not just the pragma — the same
rule every other suppression in this codebase follows.  Check whether any
other "remove" in the table is the same case (a `.c` file being told to
drop the header it implements) before applying it.

**2. The by-value forward declarations are fine — the callers are what to
watch.**  `compile_nav.h:55` takes `struct kg_buffer_handle` **by value**
(`register.h:99` only takes a pointer).  A by-value parameter of
incomplete type is legal in a declaration-only header and compiles clean
under `-Wall -Wextra -std=c23`, verified before writing this — so
`make header-check` will pass.  The completeness requirement moves to
whoever *defines or calls* the function, which is why IWYU's "add" column
tells `compile_nav.c` and `register.c` to include `bufhandle.h`.  Apply
both halves of a header/`.c` pair together, or the `.c` breaks.

`src/lisp_obj.h` names no `uint*_t`/`int*_t` at all, so its `<stdint.h>`
removal is straightforwardly correct.

---

## Order of work

Leaves first, so a header change is verified before the files that include
it move:

1. `compile_parse.h`, `lisp_obj.h`, `register.h`, `compile_nav.h`,
   `process_table.h` — headers, each followed by `make header-check`.
2. The `src/lisp_*.c` files.
3. `compile_nav.c`, `register.c`, `process_table.c`.

`src/lisp_*.c` changes are the ones that can break the disabled build:
rebuild `make WITH_LISP=0 clean all check` behind that group, not only at
the end.  `make lisp-include-check` still applies — `fe.h` may appear only
in `src/lisp_*.c` and `src/lisp_internal.h`.

Commit in those three groups rather than one 16-file commit; a mechanical
change is exactly the kind that is hard to bisect when it is one blob.

---

## Gates

- `make header-check` after each header.
- `make check` and `make WITH_LISP=0 clean all check`.
- `make lisp-include-check`, `make format-check`.
- `make fuzz-keypress` — include changes can break a target `make check`
  never links.
- `.ci/ci-06-static-analysis.sh` green: the point of the slice.  It also
  runs clang-analyzer, clang-tidy and cppcheck, so read the whole log
  rather than grepping only for IWYU.
- `JOBS=8 .ci/run-ci-steps.sh --parallel`.  ci-05 stays red until sub-plan
  E lands.

**No ratchet raises.**  Removing an include cannot raise `scc`; if
`complexity-check` moves at all, something other than includes changed.

## Completion gate

- `.ci/ci-06` green, with every finding either applied or suppressed by a
  pragma that says why.
- Every `src/*.h` still compiles standalone.
- Both `WITH_LISP` configurations green.
- Every header/`.c` pair moved together, so no caller is left without the
  complete type a forward declaration stopped providing.
- No ratchet moved.
