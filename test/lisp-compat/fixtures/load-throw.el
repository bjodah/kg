;;; A `throw' out of a loaded file, to a `catch' around the `load'.
;;; Emacs reaches that catch; kg's containment barrier is a throw wall,
;;; so the throw becomes `no-catch'.  See the load-throw-reachability row.
(throw 'load-tag 99)
