#!/usr/bin/env bash
# usage: gen_chain.sh N [linear|nonlinear]   -> rfl on stdout
N=${1:?N}; MODE=${2:-linear}
cat <<EOF
(set edge (table [edge__c0 edge__c1] (list (+ 1 (til $N)) (+ 2 (til $N)))))
(set db (datoms))
(rule (tc ?x ?y) (edge ?x ?y))
EOF
if [ "$MODE" = nonlinear ]; then
  echo '(rule (tc ?x ?z) (tc ?x ?y) (tc ?y ?z))'
else
  echo '(rule (tc ?x ?z) (edge ?x ?y) (tc ?y ?z))'
fi
cat <<EOF
(set r (query db (find ?x ?y) (where (tc ?x ?y))))
(println (count r))
EOF
