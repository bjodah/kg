;;; The blocker sequence a real package presents kg with, reproduced
;;; SYNTHETICALLY: nothing here is copied from s.el or from any other
;;; elpa package, because vendoring a GPLv3+ file as a test fixture is a
;;; licensing decision the maintainer makes and the suite's own rule is
;;; that a fixture must need nothing fetched.  What is copied is the
;;; SHAPE -- an autoload declaration in the header (s.el:34), a page
;;; separator between sections (s.el:770), and a `declare' debug spec
;;; carrying a vector literal -- in the order a load meets them.
;;;
;;; The first two are landed: the autoload declaration is accepted and
;;; inert, and the form feed below is reader whitespace.  The load
;;; therefore reaches the third and stops there, with
;;;
;;;     unsupported read syntax: vector brackets
;;;
;;; which is the CHECKED-IN EXPECTED ERROR.  PHASE 24 FLIPS THIS: when kg
;;; reads vector literals, this file loads clean, `sel-frontier-loaded'
;;; answers t, and the sel-frontier-vector-literal case, its snapshot and
;;; test_sel_frontier_vector_literal move in the same commit as the
;;; behaviour.
(autoload 'sel-frontier-absent "sel-frontier-no-such-library" "Doc." t)

(setq sel-frontier-past-autoload t)

(setq sel-frontier-past-page-break t)

(defmacro sel-frontier-when-let (spec &rest body)
  "Bind the second element of SPEC and run BODY when it is non-nil."
  (declare (debug ((form [&or symbolp stringp]) body)) (indent 1))
  (list 'if (car (cdr spec)) (cons 'progn body)))

(setq sel-frontier-loaded t)
