## Tidbits
- `C-:` (eval-expression)
- `C-s` followed by e.g. "paris" should match "Paris" (as in emacs), while search
   for "Paris" would not match all-lower-case version.
- Remove legacy constructus in `fe` (as defined by fe/doc/c-api.md)
- When entering a shell command (M-!), kg style movement (C-f, C-b, M-f, M-b) and
   manipulation (M-d, C-y, ...) should be supported.
- Support for some simple modes, e.g. visual-line-mode
- When opening (at least *scratch* buffer, maybe all fe-lisp-mode buffers once we 
  have modes in place) kg, writing "(+ 3 2)" and pressing "C-j" should yield "5" 
  on the next row (like in emacs).

## Next steps

- One/a few small fe-package(s) modes (installed to share/kg/site-lisp/), either:
  - [x] "dired-mode" clone (or something even simpler if deemed to ambitious)
        — shipped, but as a C mode (`src/dired.c`, "Dired Mode" in
        `doc/kg.1`), not an fe package: it is syscalls end to end
        (`opendir`/`stat`/`unlink`/`rmdir`), and a C mode is also there in a
        `WITH_LISP=0` build.  So the "first shipped `.fe` package" milestone
        is deliberately *not* consumed by it and remains open; it wants a
        package that is actually Lisp-shaped.
  - "magit-style" git commit mode, (see mention in doc/TODO.md)
