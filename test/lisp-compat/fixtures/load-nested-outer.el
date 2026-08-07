;;; A catch inside a loaded file receives a throw from a nested load,
;;; and this file keeps evaluating past it.
(setq load-nested-probe
      (catch 'nested-tag
        (load (if (fboundp 'expand-file-name)
                  (expand-file-name "test/lisp-compat/fixtures/load-nested-inner.el")
                "test/lisp-compat/fixtures/load-nested-inner.el"))))
(setq load-nested-probe (+ load-nested-probe 1))
