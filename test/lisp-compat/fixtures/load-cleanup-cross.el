;;; An unwind-protect cleanup in the loading frame runs while a throw
;;; crosses the load on its way to the caller's catch.
(unwind-protect
    (throw 'cross-tag 5)
  (setq load-cross-cleaned 'yes))
