# Plan: a small syntax/major-mode framework

## Why now

`add-yaml-mode.md` is the fourth specialized highlighter after Markdown,
Makefile and Git commit, and it will not be the last. Before adding another
mode, the shared plumbing that every specialized mode currently re-implements
by hand should be extracted once. Doing this first makes the YAML patch (and
every mode after it) mostly *data plus one highlighter function*, with no edits
to the dispatcher and no bespoke cross-row propagation.

This plan is a **prerequisite** for `add-yaml-mode.md`. It is deliberately
behavior-preserving: the existing Markdown / Makefile / Git-commit / generic
highlighting must stay identical (the one intentional change is that stateful
modes finally see blank rows — see §3). All of `make check`, both Lisp
configurations, sanitizers and the CI scripts must remain green.

## Problems in the current design

1. **Hardcoded dispatch.** `editor_update_syntax()` selects a specialized
   highlighter through a chain of `if (editor.syntax->flags & SHL_X) {
   x_syntax(row); return; }`. Every new mode edits this function.

2. **Duplicated cross-row propagation.** Markdown, Makefile, Git-commit and
   the generic scanner each contain their own copy of the
   "`if (row->hl_oc != oc && row->idx + 1 < editor.numrows)
   editor_update_syntax(&editor.row[row->idx + 1]); row->hl_oc = oc;`" idiom.
   It is easy to get subtly wrong (e.g. forgetting the `numrows` guard) and it
   is copied verbatim into each new mode.

3. **Empty rows skip stateful modes.** `editor_update_syntax()` returns early
   for `row->rsize == 0` *before* specialized dispatch, so a zero-length row
   never updates its `hl_oc`. Markdown fenced blocks that contain a blank line
   are already mishandled by this; YAML block scalars (which must survive blank
   lines) would be broken by it. This is the one place the framework changes
   behavior on purpose.

4. **Inconsistent major-mode setters.** `cmd_lisp_interaction_mode()` just
   assigns `editor.syntax` and does not rebuild the existing rows' highlight
   arrays or clear stale `hl_oc`. A manual `M-x yaml-mode` needs a full,
   correct re-highlight, and there is no shared helper for it.

5. **Tests index `HLDB` numerically.** `test_syntax.c` refers to `HLDB[18]`,
   `HLDB[19]`, `HLDB[21]` and guards against index drift by hand. Every
   inserted entry perturbs these.

## 1. Data-driven dispatch via a highlighter function pointer

Add a highlighter hook to `struct editor_syntax` in `src/def.h`:

```c
struct editor_syntax {
	char *name;
	char **filematch;
	char **keywords;
	char singleline_comment_start[5];
	char multiline_comment_start[5];
	char multiline_comment_end[5];
	int flags;
	void (*highlight)(erow *row); /* NULL => generic keyword scanner */
};
```

Give the existing specialized `HLDB` entries their function
(`markdown_syntax`, `makefile_syntax`, `gitcommit_syntax`); leave `highlight`
as `NULL`/`0` for every generic language entry. Because the struct is
initialized with positional aggregates, add the field at the end and let the
generic entries default to `NULL`.

Rewrite the top of `editor_update_syntax()` so dispatch is a single indirect
call rather than a flag chain:

```c
if (editor.syntax == NULL)
	return;                 /* everything stays HL_NORMAL */

int old_oc = row->hl_oc;
if (editor.syntax->highlight)
	editor.syntax->highlight(row);
else
	generic_keyword_scan(row);  /* the current inline generic body */

syntax_propagate_downstream(row, old_oc);   /* see §2 */
```

Extract the current inline generic scanner body (keywords, single/multi-line
comments, numbers, strings) into a `static void generic_keyword_scan(erow *)`
so it is just another highlighter and the dispatcher has no special case.

The `SHL_*` flag bits stay for the few behavioral predicates that are not
dispatch (e.g. the Git-commit check in `editor_is_gitcommit_buffer()`), but new
modes no longer need a dispatch flag at all. A new mode = one `HLDB` row + one
highlighter function.

## 2. Centralize cross-row state propagation

Introduce one helper and delete the copies:

```c
/* Called by the framework after a row is highlighted. If the row's trailing
 * state changed, re-highlight following rows until their state stabilizes,
 * iteratively (never recursively) so pathological files cannot blow the
 * stack. */
static void syntax_propagate_downstream(erow *row, int old_oc);

/* Public entry point for whole-span rebuilds (mode switch, large edits). */
void editor_rehighlight_from(int start_idx);
```

`syntax_propagate_downstream()` compares `row->hl_oc` to `old_oc` and, if it
changed and there is a next row, walks forward: highlight row *n+1*; if *its*
`hl_oc` also changed, continue to *n+2*; stop as soon as a row's state is
unchanged or the buffer ends. Implement the forward walk with a loop, not
recursion (item addresses the current recursive `editor_update_syntax` chain
and the deep-propagation risk the YAML plan flags).

Each highlighter's contract becomes: *set `row->hl` for the row and set
`row->hl_oc` to this row's trailing state; do not chase the next row yourself.*
Remove the hand-rolled "update next row" tails from `markdown_syntax`,
`makefile_syntax`, `gitcommit_syntax` and `generic_keyword_scan`.

Backward re-triggering that a mode does for its *own* reasons (Markdown's
setext underline re-highlighting the line above) stays inside that mode — the
framework only owns forward state flow.

Guard against re-entrancy: `editor_rehighlight_from()` and
`syntax_propagate_downstream()` share a loop, so a re-highlight triggered from
within a highlighter must not recurse into the framework's own forward walk.
Because propagation is now iterative and driven only from the dispatcher, a
highlighter should call plain `editor_update_syntax()` for backward touches and
never the propagation helper.

## 3. Let stateful modes see empty rows

Restructure the `row->rsize == 0` handling so the highlight array is freed but
stateful dispatch still runs:

```c
if (row->rsize == 0) {
	free(row->hl);
	row->hl = NULL;
	if (editor.syntax && editor.syntax->highlight) {
		int old_oc = row->hl_oc;
		editor.syntax->highlight(row);   /* state propagation only */
		syntax_propagate_downstream(row, old_oc);
	} else {
		row->hl_oc = 0;
	}
	return;
}
```

Every highlighter must therefore tolerate `rsize == 0` (no `row->render`
dereference on a zero-length row) and, for an empty row, do nothing except
compute the state it forwards. Markdown gains correct fenced-block handling
across blank lines as a result; add a Markdown regression (blank line inside a
```` ``` ```` fence stays `HL_STRING`) to prove the change is an improvement,
not a regression.

## 4. A uniform major-mode setter

Add to `src/syntax.c` (declared in `src/def.h`):

```c
struct editor_syntax *syntax_find_by_name(const char *name);
void editor_set_syntax(struct editor_syntax *syntax);
void editor_rehighlight_all(void);
```

* `syntax_find_by_name()` linear-scans `HLDB` by `name` (also usable by tests,
  §5). Returns `NULL` if absent.
* `editor_rehighlight_all()` clears every row's `hl_oc`, then highlights rows
  top-to-bottom (top-down so forward propagation is a no-op second pass).
* `editor_set_syntax()` assigns `editor.syntax`, calls
  `editor_rehighlight_all()`, and preserves cursor/viewport. It leaves the
  buffer↔slot syntax association handling to the existing save-state path.

Migrate `cmd_lisp_interaction_mode()` onto `editor_set_syntax()` so manual mode
changes rebuild highlight state consistently. This both fixes the existing
"lisp-interaction-mode does not re-highlight" gap and gives `yaml-mode` a ready
helper.

## 5. Name-based syntax lookup in tests

Before more entries land, replace numeric `HLDB[n]` indexing in `test_syntax.c`
with `syntax_find_by_name("Markdown")` etc., using the production
`syntax_find_by_name()`. Add whatever `HLDB` terminator/count export the lookup
needs (a trailing sentinel `{0}` entry or the existing `HLDB_ENTRIES` macro
exposed via a small accessor). After this, inserting a mode never edits an
index literal.

## Test / verification plan

* Native `test_syntax.c`: keep all current assertions green after the refactor
  (behavior-preserving), converted to name-based lookup. Add the Markdown
  blank-line-in-fence regression from §3.
* Add a focused test that a highlighter registered only via the `highlight`
  pointer (no `SHL_*` dispatch flag) is actually invoked — this is the property
  the YAML plan relies on.
* Add a test that `editor_set_syntax()` rebuilds `hl` for pre-existing rows
  (drive it the way `lisp-interaction-mode` is exercised, or a small unit that
  sets syntax on a populated buffer and checks a known row's `hl`).
* Confirm iterative propagation: a buffer where row 0's edit flips state for
  many following rows must re-highlight all of them without unbounded
  recursion (a large synthetic Markdown fence or generic multiline comment).

## Suggested commit sequence

1. **Add the `highlight` pointer and convert dispatch.** Extract
   `generic_keyword_scan()`; point the specialized `HLDB` entries at their
   functions; dispatcher becomes one indirect call. No behavior change.
2. **Centralize propagation.** Add `syntax_propagate_downstream()` /
   `editor_rehighlight_from()`; delete the per-mode "update next row" tails;
   make propagation iterative.
3. **Empty-row dispatch fix + Markdown blank-line regression.**
4. **Major-mode API + migrate `lisp-interaction-mode`.**
5. **Name-based HLDB lookup in tests.**

Each commit is independently green and free of YAML. `add-yaml-mode.md` starts
after commit 5.

## What this deliberately does not do

* No change to the terminal color set or `HL_*` token classes.
* No generalized "buffer-local minor modes" system — this is only about syntax
  highlighting and the single active major mode already modeled by
  `editor.syntax`.
* No move to a pattern/regex-driven highlighter; specialized modes remain hand
  written C, which the repo already prefers.
