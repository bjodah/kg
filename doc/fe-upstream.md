# Fe upstream

kg embeds the core of [Fe](https://github.com/bjodah/fe) through the `fe/` git
submodule. The submodule tracks the **`analyzers-etc` branch** of
`github.com:bjodah/fe`; that branch name is the pin. The exact commit is
recorded automatically by git as part of `fe/` being a submodule — the
superproject's tree stores the SHA the working tree is checked out at, and
`git submodule status` prints it — so no commit hash is repeated here. A hash
written into prose only goes stale, as it did before this document was
rewritten.

The supported embedding interface is `FE_API_VERSION 1`; `src/lisp.c` asserts
it at compile time.

Fe is MIT licensed. Copyright belongs to rxi and Chris Palmer; the complete
license text is in `fe/LICENSE`.

kg compiles only `fe/fe.c` and its public header `fe/fe.h`. The `fex*` files,
`auto.*`, and `main.c` are deliberately excluded so Fe's optional I/O,
process, regular-expression and time extensions are not exposed. The maths
natives (`sin`, `sqrt`, `expt`, …) are in `fe.c` itself and therefore are
available. Only `src/lisp.c` may include `fe.h`.

To update Fe:

1. Fetch `origin` in `fe/` and check out the tip of `analyzers-etc` (or the
   branch you are moving the pin to).
2. Review the complete submodule diff, including the kg-side divergences
   listed below — they live on that branch and must survive the update.
3. Confirm `FE_API_VERSION` and adapt kg if it changed.
4. Rerun `fe/test.sh` (or `make -C fe check`) and kg's full CI pipeline,
   including `.ci/ci-08-with-lisp-0.sh`.
5. Commit the submodule pointer in the superproject so the recorded SHA moves
   with the branch.

## kg-side divergences from upstream rxi/fe

These changes live on `bjodah/fe`'s `analyzers-etc` branch. They exist because
kg presents Fe to users as an Emacs Lisp dialect, and the remaining Emacs
surface is bought in kg's own prelude in `src/lisp.c`. Anything that could be
done in the prelude was done there instead.

| Divergence | Why | Cost |
| --- | --- | --- |
| `if` is Emacs Lisp's `(if COND THEN ELSE...)`, with the trailing forms an implicit `do` | Upstream read them as an alternating elif chain, so `(if c a b1 b2)` silently evaluated `b1` as a *condition* and never ran `b2`. Two forms cannot coexist in one language; the elisp one wins. | `scripts/concatenate.fe` and `scripts/life.fe` were rewritten as nested `if`s; `doc/language.md` updated |
| The `fn` primitive's canonical name is `lambda`; `fn` remains bound to the same primitive | `(lambda (x) …)` is what elisp users write, and closures now *print* as `(lambda …)` rather than `(fn …)` | one entry in `primitive_names`, a `primitive_aliases` table, `type_names[FeTFn]`, one string in `FeWrite` |
| Reader macros `` `x ``, `,x`, `,@x` read as `(quasiquote x)`, `(unquote x)`, `(unquote-splicing x)` | Backquote is the difference between writing macros and fighting them. The *semantics* stay in kg's prelude; only the punctuation had to move into the reader. | `` ` `` and `,` became symbol delimiters (nothing in fe or kg used them inside symbols) |
| Reader macro `#'x` reads as plain `x` | Upstream read `#'car` as a symbol named `#'car`, which evaluated to `nil` *silently*. Fe has one namespace, so the elisp function quote is the identity. | `#` stays an ordinary symbol character; only the two-character `#'` is special |
| Calling a non-function whose head is a symbol raises `void-function NAME` | Upstream's `tried to call non-callable value` never said which name was unbound | one helper in `Evaluate` |
| `GcStackSize` 512 → 4096 | The GC stack, not the C stack, bounds recursion: upstream died at about 70 frames, which is too few to write ordinary list code. Now about 450. | `FeMinimumArenaSize()` grew by 28 KiB to 36608 bytes; `test_api.c`'s fixed arena was raised to 64 KiB. kg allocates 1 MiB, so a `KG_LISP_ARENA_SIZE` override below ~68 KiB now fails to open a context. |

Deliberately **not** changed in `fe.c`, and why:

- **Quasiquote semantics.** `quasiquote` is a prelude macro in `src/lisp.c`, so
  it can change without moving the pin. The core only learns the punctuation.
- **`?a` character literals.** `FeReadFn` yields one byte at a time and Fe has
  no character type, so `?é` would silently read as the first UTF-8 byte —
  reintroducing exactly the silent wrongness `#'` was fixed to remove.
  `(string-to-char "é")` is exact and already available.
- **`=` as numeric comparison.** Possible (it is an ordinary global), but it
  would permanently retire `=`-as-assignment, and any stale `(= x 1)` would
  then silently compute a boolean. `setq` is the spelling to use; `=` stays
  assignment.
- **Dynamic binding, `unwind-protect`, `condition-case`, vectors, hash tables,
  keyword arguments, a byte compiler.** All need new object types or a real
  non-local exit mechanism in `Evaluate`. Out of scope.
