;;; The blocker sequence a real package presents kg with, reproduced
;;; SYNTHETICALLY: nothing here is copied from s.el or from any other
;;; elpa package, because vendoring a GPLv3+ file as a test fixture is a
;;; licensing decision the maintainer makes and the suite's own rule is
;;; that a fixture must need nothing fetched.  What is copied is the
;;; SHAPE -- an autoload declaration in the header (s.el:34), a page
;;; separator between sections (s.el:770), and a `declare' debug spec
;;; carrying a vector literal -- in the order a load meets them.
;;;
;;; All three are landed now.  The autoload declaration is accepted and
;;; inert (Phase C1), the form feed below is reader whitespace (Phase C2),
;;; and Phase 24 gave fe the vector reader, so this file LOADS CLEAN:
;;; `sel-frontier-loaded' answers t and the macro is defined.  It stopped
;;; at the third with
;;;
;;;     unsupported read syntax: vector brackets
;;;
;;; until that commit.  The file stays in the suite as the cheapest
;;; regression guard for all three blockers at once.
(autoload 'sel-frontier-absent "sel-frontier-no-such-library" "Doc." t)

(setq sel-frontier-past-autoload t)

(setq sel-frontier-past-page-break t)

(defmacro sel-frontier-when-let (spec &rest body)
  "Bind the second element of SPEC and run BODY when it is non-nil."
  (declare (debug ((form [&or symbolp stringp]) body)) (indent 1))
  (list 'if (car (cdr spec)) (cons 'progn body)))

(setq sel-frontier-loaded t)
