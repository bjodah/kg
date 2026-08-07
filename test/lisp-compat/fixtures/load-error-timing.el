;;; Form 1 must RUN before form 2's reader error surfaces: a load is
;;; read and evaluated incrementally, one form at a time, in both
;;; dialects.  See the load-dynamic-extent row.
(setq load-timing-probe 41)
(unclosed
