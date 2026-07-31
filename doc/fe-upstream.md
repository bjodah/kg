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

## The nested tiny-regex-c submodule

The pin chain is **kg → fe → tiny-regex-c**. `fe/.gitmodules` pins
`tiny-regex-c` on the **`adapt-to-fe` branch** of `github.com:bjodah/tiny-regex-c`,
and moving that pin means moving fe's gitlink and then kg's, in that order.

This is not at arm's length. kg compiles `fe/tiny-regex-c/re.c` **directly**
into the editor (`Makefile`'s `$(OBJDIR)/tiny_regex.o`, plus the regex fuzzer
and the differential driver, with `-Ife/tiny-regex-c` on both `CFLAGS` and
`FE_CFLAGS`), so every engine change ships to kg users whether or not Fe's own
`fex_re.c` is built. It is also compiled in **both** `WITH_LISP` configurations.
`src/regex.c` and `src/regex.h` are kg's only consumers of `re.h`.

To move the pin: land the change on `adapt-to-fe` with the submodule's own
`make check` and `.ci/run-ci-steps.sh` green, move fe's gitlink with
`make -C fe check`, then move kg's and run `make check`,
`make check-regex-differential`, `make fuzz-regex-seed-replay`,
`.ci/run-ci-steps.sh` and `.ci/ci-08-with-lisp-0.sh`.

Growing `re_status` is an ABI change that has to move `fe/fex_re.c`,
`src/regex.c` and `test/regex_differential.c` in the same pin step; prefer
reusing an existing value.

### kg-visible divergences from upstream kokke/tiny-regex-c

| Divergence | Why | Cost |
| --- | --- | --- |
| Emacs-style escaped operators: `\(...\)`, `\|`, `\{n,m\}`; bare `(`, `)`, `\|`, `{` are literal | kg presents regexps as Emacs' | patterns in kg's docs, tests and Lisp are Emacs-shaped |
| The matcher steps by UTF-8 character, not by byte | `å*` must repeat `å`, and `[åä]` must not match the `0xC3` they share | spans and `start_offset` stay byte offsets; `re.h` says so |
| A construct the engine cannot honour is `RE_STATUS_BAD_PATTERN`, never literal text | `[[:blank:]]` used to match `a`, out of the letters spelling the class name | an exact documented subset in `re.h`; kg surfaces it as `KG_REGEX_BADPAT` |
| An unclosed `\(`, an unmatched `\)` and an unterminated bracket expression are bad patterns; a `]` in first position is a member | `\(\(a\)` and `[a` used to compile and quietly mean something else | one compile-time parse stack replaces two scans over the half-written program |
| A quantifier on an already-quantified atom (`a++`, `a\{2\}\{3\}`) is a bad pattern | Emacs folds the two into one; this engine has no faithful spelling for the composition, and used to compile a node that could never match | the one place kg is stricter than Emacs; `utils/regex_differential.py` never generates it |
| `*`, `+`, `?` and `\{` with nothing to repeat are the literal characters | Emacs reads them that way at the start of a pattern, group or alternative | `\(?` is accepted as a literal `?` where Emacs reserves it for shy groups |
| Caller storage must be `RE_STORAGE_ALIGNMENT`-aligned or the compile is refused | the program's multi-byte fields are read in place; a misaligned buffer aborted under UBSan | `struct kg_regex` declares `alignas(RE_STORAGE_ALIGNMENT)` rather than relying on its own layout |
| The matching budget is per execution, with `re_exec_with_options()` for per-call limits and cancellation | it used to live in file-scope statics, so a nested or concurrent match spent the running one's allowance | kg still calls plain `re_exec()`; the options are available for a future C-g-aware search |
| Reaching the group-repetition ceiling is `RE_STATUS_TOO_COMPLEX` | it was a false no-match, and for `\(a\)*` a match shorter than the pattern asked for | kg reports `KG_REGEX_TOO_COMPLEX`, which the search UI already distinguishes from "not found" |

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
