(set-option :produce-models true)
(set-logic QF_UFLIAFS)
(set-info :status sat)
(declare-fun X () (Set Int))
(assert (= X (insert 1 2 (singleton 3))))
(check-sat)
;(get-model)
