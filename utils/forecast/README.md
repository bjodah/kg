# The forecast corpus

Input to `utils/forecast_audit.py`, whose output is `AUDIT.md` beside
this file.  The audit asks one question -- *which names does the Lisp we
want to write reach for, and which of those does kg not have?* -- and the
corpus is the "Lisp we want to write" half of it.

Three kinds of file are in the corpus, and the distinction matters when
reading the report:

* **`lisp/*.el`** -- kg's own shipped Lisp (the prelude, `auto-fill.el`,
  the `pipeline` pair).  Real, running code.  It should contribute no
  MISSING names at all; when it does, something shipped is calling a name
  nothing defines.
* **`target-init.el`** -- a hand-written, realistic user init file.  It
  is the corpus's **floor**: Phase 15's definition of done requires it to
  load clean under `test/kgbatch`, so every name it calls must exist.
  `make forecast-check` does not enforce that (it runs nothing);
  `test/test_lisp.c`'s `test_forecast_target_init` does.
* **`forecast-*.el`** -- hand-written package *sketches*.  They are the
  corpus's **ceiling**: deliberately written the way one would write the
  package for Emacs, without checking first whether kg has the names.
  They do not load, they are not installed, and nothing evaluates them.
  Their whole job is to name what is missing.

Everything here is hand-written.  Nothing is vendored from GNU Emacs or
from any other package: a licence question is not worth the convenience,
and a sketch written *for this audit* states our intent, where a
vendored file would state someone else's.

## Adding to the corpus

Drop a `.el` file in this directory and run `make forecast-audit`.  A new
sketch that adds MISSING names is the tool working, not a failure -- the
gate (`make forecast-check`, part of `make check`) only requires that
`AUDIT.md` matches what the tool produces from the tree as it stands.
