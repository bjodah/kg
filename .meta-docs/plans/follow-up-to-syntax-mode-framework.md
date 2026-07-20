# Follow-up to the syntax/major-mode framework

Review of the implemented framework (function-pointer dispatch, centralized
`syntax_propagate_downstream`, empty-row dispatch, `editor_set_syntax` /
`editor_rehighlight_all` / `editor_rehighlight_from`, name-based test lookup).

The core design is sound and behavior-preserving: dispatch is now data-driven
via `editor_syntax.highlight`, propagation is iterative rather than recursive,
specialized modes no longer copy the "re-highlight the next row" idiom, and the
Markdown blank-line-in-fence case is fixed and tested. The items below are the
deficiencies that remain.

---

## Findings

### [P1] `editor_rehighlight_all()` is quadratic

```c
void editor_rehighlight_all(void)
{
	for (j ...) editor.row[j].hl_oc = 0;
	for (j ...) editor_update_syntax(&editor.row[j]);   /* <-- */
}
```

`editor_update_syntax()` runs `editor_update_syntax_row_only()` **and**
`syntax_propagate_downstream()`. Called in a top-down loop, every row that
changes its trailing state re-highlights the entire tail, and the outer loop
then visits those same rows again. For a file that is one long block comment or
one long fenced code block, `M-x lisp-interaction-mode` / any
`editor_set_syntax()` becomes O(n²) — a multi-thousand-line file can stall on a
mode switch.

Because the loop already walks strictly top-to-bottom, each row sees the final
state of the row above it after a single pass; downstream propagation is
redundant here.

**Fix:** call `editor_update_syntax_row_only()` in the second loop, not the
propagating `editor_update_syntax()`. Keep the separate `hl_oc = 0` reset pass.
Add a large single-block file to the tests as a guard (correctness must be
identical; the change is purely to drop the quadratic factor).

---

### [P2] Generic multiline comments are still lost across blank lines

`editor_row_has_open_comment()` returns 0 for any empty row (it inspects
`row->hl`, which is `NULL` for a zero-length row), and the empty-row branch of
`editor_update_syntax_row_only()` hardcodes `row->hl_oc = 0` for the generic
path (`highlight == NULL`). So for C/JS/CSS-style languages:

```c
/* comment
                       <- blank line
still comment */
```

the line after the blank is highlighted as code, not comment.

This is **pre-existing** (the old early-return also never propagated comment
state through blank rows), but the framework introduced the very mechanism that
makes fixing it natural — specialized modes now propagate `hl_oc` through empty
rows, and only the generic path opts out. The result is an inconsistency:
Markdown fences survive blank lines, C comments do not.

**Fix:** in the empty-row generic branch, inherit the previous row's trailing
comment state instead of forcing 0 — e.g.
`row->hl_oc = (row->idx > 0) ? editor.row[row->idx - 1].hl_oc : 0;` — so an open
multiline comment carries across blank lines the same way a fence does. Add a
C-comment-across-blank-line regression. Verify it does not resurrect stale state
on the first non-blank line after the comment closes.

---

### [P2] Backward re-trigger still uses the propagating entry point

`markdown_syntax()` re-highlights the row above a setext underline with the full
`editor_update_syntax(&editor.row[row->idx - 1])`, which runs its own downstream
propagation. Invoked from inside a highlighter that the framework called via
`editor_update_syntax_row_only()`, this nests propagation within propagation:
during `editor_rehighlight_all()` every setext underline re-highlights the line
above *and* re-propagates the whole tail from there, compounding the P1 cost,
and it means the "iterative, never recursive" guarantee only half holds.

**Fix:** a one-row backward touch should use `editor_update_syntax_row_only()` —
the framework owns forward flow, so a highlighter's backward correction never
needs to re-propagate. (Contract: highlighters do their own *backward* touches
row-only and never call the propagating entry point.)

---

### [P2] Dead dispatch flags left after the pointer migration

Dispatch moved to `editor_syntax.highlight`, but `SHL_MARKDOWN` and
`SHL_MAKEFILE` are still set in `HLDB` and are now read nowhere.
`SHL_GITCOMMIT` is the only flag still consulted (in `syntax_is_git_commit()`).

**Fix:** finish the migration — drop `SHL_MARKDOWN`/`SHL_MAKEFILE`, and either
give Git-commit its own predicate not tied to a dispatch flag (e.g. compare
`editor.syntax->highlight == gitcommit_syntax`, or a dedicated `is_gitcommit`
bit that is clearly a *behavioral* flag, not dispatch) or explicitly document
`SHL_GITCOMMIT` as behavioral. Leaving two dead bits in every table row is the
kind of drift the framework was meant to remove.

---

### [P2] `editor_rehighlight_from()` is dead API

It is exported and documented as the public span-rebuild entry, but nothing
calls it. Edits still flow through `editor_update_row()` →
`editor_update_syntax()` per row, and `editor_del_row()` (see P3) does not use
it either.

**Fix:** either wire it in where a multi-row rebuild is wanted (e.g. after
block operations, paste, or row deletion) so there is one canonical span entry,
or remove it until there is a caller. Dead exported API invites divergent
call sites later.

---

### [P3] `editor_set_syntax()` does not update the buffer slot immediately

It assigns `editor.syntax` and rebuilds rows, but leaves
`buflist[buf_current].syntax` stale until the next `buf_save_current_state()`.
Normal buffer switches call that first, so the manual mode does persist, but any
path that reads the slot's syntax before a save (buffer-list rendering, another
window's mode line) sees the pre-switch value.

**Fix:** set `buflist[buf_current].syntax = syntax` inside `editor_set_syntax()`
so the slot and the live editor agree immediately. Low risk, removes an implicit
ordering dependency.

---

### [P3] Empty-row highlighter contract is unenforced

`makefile_syntax()` returns early for a zero-length row without setting
`hl_oc`. It is harmless today (Makefile mode keeps no cross-row state), but the
framework's contract — "a highlighter must set `row->hl_oc` for empty rows too"
— is silently unmet, a trap for the next stateful mode (YAML) copied from
Makefile as a template.

**Fix:** either have the framework reset `hl_oc = 0` for empty rows *before*
dispatching to a specialized highlighter (highlighter then only overrides when
it has state to carry), or document the contract prominently and make
`makefile_syntax()` set `hl_oc = 0` explicitly on the empty-row path.

---

### [P3] Redundant guards and cruft from the empty-row retrofit

`markdown_syntax()` now contains doubled guards such as
`if (len > 0 && p[0] == '#') { if (len > 0) memset(...); }`. The inner
`if (len > 0)` is unreachable-when-false. Cosmetic, but it is new noise in a
codebase that values minimalism.

**Fix:** collapse the doubled `len > 0` guards; a single guard per branch.

---

### [P3] `editor_del_row()` leaves stale highlight state downstream

Deleting a row shifts `idx` values and decrements `numrows` but re-highlights
nothing. When the deleted row opened a comment/fence, correctness currently
relies on the caller subsequently running `editor_update_row()` on a joined row
(which propagates). A deletion path that does not join would leave the following
rows stale.

This is **pre-existing**, but it is the natural counterpart to the insert-side
fix (`editor_insert_row` now bumps `numrows` before highlighting so propagation
sees the new row). **Consider** an explicit `editor_rehighlight_from(at)` after
`editor_del_row()` — which would also give the dead API from the earlier finding
a real caller.

---

## Test coverage

Present and good: name-based lookup, custom-pointer dispatch, `editor_set_syntax`
rebuild, Markdown blank-line-in-fence, and a 500-row propagation case.

Missing:

* **Generic comment across a blank line** (would fail today; guards the P2 fix).
* **`editor_rehighlight_all` on a large single-block file** — correctness plus a
  soft bound proving it is not quadratic (e.g. it must not re-highlight each row
  more than a small constant number of times; a counter hook or a generous time
  budget).
* **Propagation on row deletion** — delete the fence-opening line and assert the
  formerly-fenced rows revert to normal highlighting.
* **`editor_set_syntax` persistence across a buffer switch** (drives P3-slot).
* **Makefile empty-row** — a blank line between rules does not crash and leaves
  neighbours correct.
* **Setext-recursion stress** — many consecutive setext-style lines to show the
  backward re-trigger does not blow up after the P2 backward-touch fix.

---

## Overall assessment

The migration achieved its goal: adding a mode is now a table row plus one
function, propagation is centralized and iterative, and the empty-row hole is
closed for stateful modes. The one item worth fixing before the tree grows is
the **quadratic `editor_rehighlight_all`** (P1); the P2 group is a mix of one
pre-existing correctness gap the framework can now close cheaply and two
cleanups that finish the migration it started. None of the findings block
`add-yaml-mode.md`, but P2 (backward-touch), P3 (empty-row contract) and the
contract documentation should land first, because YAML is exactly the stateful,
block-scalar mode that will exercise all three.
