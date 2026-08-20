# What a NUL does at kg's Lisp boundary

Taken at Phase 25.2 of kg's Elisp data-model program
(`doc/plans/2026-08-20-elisp-data-model-phase25-execution.md`), as the kg half
of `fe/doc/payload-pointer-census.md`'s section D.

That census asked a LIFETIME question of every kg site that reads an fe
string, and Phase 25 answered it with a shrug: `fe.h` returns no pointer into
object storage and never did, `FeStringByteLength` + `FeCopyStringBytes` copy
into a buffer the caller owns, and neither call's meaning changed when a
string became a header over a moving payload block.  So there was no
migration to do, and this file is not about the question the census asked.

It is about the one the pin actually raised.  **Since `FE_LANGUAGE_VERSION`
17 a fe string may contain a NUL**, so a kg site that copies one out and then
treats the copy as a C string does not fail: it silently answers a shorter
string.  `copy_fe_string()` hands back BOTH the bytes and the byte count, so
every site already has what it needs; the question per site is whether it
USES the count, and where it does not, whether truncating is the right
answer.

Both answers are legitimate.  kg is a text editor: a buffer name, a hook
name, a file path and an `execve` argument are C strings by the time anything
outside kg sees them, and a NUL cannot survive in them however carefully kg
carries it.  What must not happen is a site that means to carry bytes and
loses them by accident.

## The rule

* A site that hands the bytes to something that takes a POINTER AND A LENGTH
  -- a buffer edit, a `memcmp`, a byte loop, `FeMakeStringBytes` -- carries
  the length.  There is no reason not to: it already has it.
* A site that hands them to something that takes a C STRING -- `open`,
  `execve`, `snprintf`, an identifier table keyed by `strcmp` -- truncates,
  and that is the correct answer rather than a tolerated one.  The row below
  says which of those it is.
* A site that BUILDS an fe string uses `FeMakeStringBytes` with the length it
  already has, never `FeMakeString` over the copy.  This is the direction the
  phase actually changed; the table at the end is the list.

## The twenty `copy_fe_string()` sites (census row D1)

Six carry the length, one reads a single byte, and thirteen truncate on
purpose.

| site | what it does with the bytes | verdict |
| --- | --- | --- |
| `lisp_io.c:568` `insert` | `kg_edit_user(b, ..., text, length)` -- a buffer edit takes a pointer and a length | **carries the length** |
| `lisp_io.c:653` `replace-region` | the same gateway call | **carries the length** |
| `lisp_io.c:523` `format`/`message` | the FORMAT string is walked by `length`, byte by byte | **carries the length** (what it BUILDS is table 3's row) |
| `lisp_motion.c:317` `skip-chars-forward`/`-backward` | `lisp_skip_parse(context, spec, (int)spec_length, &set)` | **carries the length** |
| `lisp_search.c:261` `search-forward`/`-backward`, `re-search-*` | a LITERAL search compares with explicit lengths, which the file's own header comment already said | **carries the length** |
| `lisp_string.c:37` `lisp_string_argument` | returns the count to every string native; each of them uses it | **carries the length** |
| `lisp_cmd.c:994` the keyword test | reads `text[0]` under a `length > 0` guard | **binary-safe**, it reads one byte |
| `lisp_process.c:414` `start-shell-command` | `req.command` reaches `execve` | **truncates by design**: an argv string is NUL-terminated by the kernel's own contract, so no representation kg chooses can pass a NUL through |
| `lisp_hooks.c:449`, `:487`, `:527` `add-hook`, `remove-hook`, `run-hooks` | `snprintf` into `hook_name[]`, then a `strcmp` lookup | **truncates by design**: a hook name is an identifier. The worst case is aliasing -- `"a\0b"` names the hook `a` -- not corruption |
| `lisp_cmd.c:320` `copy_command_name` | copies `length + 1` bytes into a frame buffer, then a registry lookup by name | **truncates by design**: same, a command name is an identifier |
| `lisp_obj.c:629` `get-buffer`/`set-buffer`/`kill-buffer` | the same shape, then a buffer-name lookup | **truncates by design**: kg's buffer names are C strings end to end, in the mode line and in `bufmgr` |
| `lisp_require.c:41` `provide`/`featurep` | `snprintf` into the feature table | **truncates by design**: an identifier |
| `lisp_require.c:150` `add-to-load-path` | a directory pushed onto the load path | **truncates by design**: a filesystem path |
| `lisp_io.c:970` `internal--load-begin` | `snprintf` into `path[PATH_MAX]`, then `fopen` | **truncates by design**: a filesystem path |
| `lisp_io.c:1074` `internal--resolve-load` | the same, plus `strchr(name, '/')` | **truncates by design**: a filesystem path |
| `lisp_core.c:314` `describe_callable_failure` | the symbol's name interpolated into a diagnostic | **truncates by design**: a message for a human, and only a symbol NAME can reach it |
| `lisp_core.c:1265` `copy_bounded` | an interactive spec's prompt and answers | **truncates by design**: prompt text |
| `lisp_search.c:627` `looking-at` | the PATTERN handed to `kg_regex_compile` | **truncates by design**, and it is the regex engine's rule rather than this seam's: `src/regex.h` takes NUL-terminated patterns and subjects, which `doc/lisp-api.md` records as one of three engine-inherited divergences |

## The other census rows, D2-D8

| # | site | verdict |
| --- | --- | --- |
| D2 | `lisp_search.c` `lisp_pattern_and_subject` | Both halves are copied by length into one block and then NUL-terminated for the engine. **Truncates by the engine's contract**, as D1's `looking-at` row does, and for the same recorded reason |
| D3 | `lisp_search.c` `native_regexp_quote` | The quoting loop already ran over `length` bytes; only the RESULT was built with `FeMakeString`. **Fixed** (table 3) |
| D4 | `lisp_string.c` `lisp_concat_bytes` / `native_concat` | Both passes are length-driven; only the result was built with `FeMakeString`. **Fixed** (table 3) |
| D5 | `lisp_string.c` `native_string_equal` | Two lengths, one allocation, `memcmp`. **Already binary-safe**, and the case `(string= "a\0b" "a\0c")` proves it: nil, not t |
| D6 | `lisp_process.c` `start-process` argv build | **Truncates by design**, D1's `start-shell-command` row's reason |
| D7 | `lisp_prompt.c` `copy_string_argument` | Prompt text onto the caller's frame. **Truncates by design** |
| D8 | `lisp_cmd.c`, `lisp_core.c`, `lisp_word.c`, `lisp_prompt.c` -- `FeToString` into a stack buffer, then `strcmp` | **Truncates by design**, and hardly reachable: `FeToString` is the PRINTER, which escapes a NUL to `\000` rather than emitting it. Only a symbol's name arrives raw |

## The other direction: what kg BUILDS

This is what Phase 25.2 changed.  Nine sites had the bytes and their count
and threw the count away; each now uses `FeMakeStringBytes`.  Everything else
in kg that calls `FeMakeString` builds from a C string that never had a
length -- a buffer name, a filename, a path, a command summary, a rendered
number -- and has nothing to carry.

| site | what it builds | what it did before |
| --- | --- | --- |
| `lisp_string.c:145` `substring` | the slice | wrote a NUL at the end and passed `text + from` |
| `lisp_string.c:192` `concat` | the joined bytes | `FeMakeString` over the block |
| `lisp_string.c:281` `char-to-string` | the encoded character | refused 0 outright |
| `lisp_string.c:380` `upcase`/`downcase`/`capitalize` | the converted copy | `FeMakeString` over it |
| `lisp_string.c:553` `make-string` | N copies of one character | refused 0 outright |
| `lisp_io.c:539` `format` | the formatted output | `FeMakeString` over the buffer; `%c` refused 0 |
| `lisp_search.c:593` `regexp-quote` | the quoted rendering | `FeMakeString` over it |
| `lisp_buffer.c:485` `buffer-substring` | the buffer text between two positions | `FeMakeString` over the span. A file kg opened may hold a NUL, so this one truncated real user data |
| `lisp_process.c:162` a process filter's argument | the bytes the process wrote | `FeMakeString` over them, though the process table had already answered the length |

`message` is the deliberate exception in that direction: it hands the same
formatted text to the echo area, which takes a C string, so it truncates and
says so where it does it.

## What is deliberately not solved here

kg has ONE string type and it is bytes.  Emacs has two -- unibyte and
multibyte -- and the difference shows up in exactly the places
`test/lisp-compat`'s `string25-*` rows record: `aref`/`aset` index bytes
here and characters there for multibyte text, and fe's writer escapes a NUL
to `\000` where Emacs emits the byte.  None of that is a truncation
question, and none of it is on this page.
