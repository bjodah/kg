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
  - "dired-mode" clone (or something even simpler if deemed to ambitious)
  - "magit-style" git commit mode, (see mention in doc/TODO.md)
