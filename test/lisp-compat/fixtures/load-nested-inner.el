;;; The inner half of load-nested-inner-throw: thrown across this load
;;; to the catch in the outer fixture.
(throw 'nested-tag 7)
