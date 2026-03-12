LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ptab (id integer, val text) WITH (autovacuum_enabled = false);

SET pg_plan_advice.always_store_advice_details = false;

-- Not prepared, so advice should be generated.
EXPLAIN (COSTS OFF, PLAN_ADVICE) 
SELECT * FROM ptab;

-- Prepared, so advice should not be generated.
PREPARE pt1 AS SELECT * FROM ptab;
EXPLAIN (COSTS OFF, PLAN_ADVICE) EXECUTE pt1;

SET pg_plan_advice.always_store_advice_details = true;

-- Prepared, but always_store_advice_details = true, so should show advice.
PREPARE pt2 AS SELECT * FROM ptab;
EXPLAIN (COSTS OFF, PLAN_ADVICE) EXECUTE pt2;

-- Not prepared, so feedback should be generated.
SET pg_plan_advice.always_store_advice_details = false;
SET pg_plan_advice.advice = 'SEQ_SCAN(ptab)';
EXPLAIN (COSTS OFF) 
SELECT * FROM ptab;

-- Prepared, so advice should not be generated.
PREPARE pt3 AS SELECT * FROM ptab;
EXPLAIN (COSTS OFF) EXECUTE pt1;

SET pg_plan_advice.always_store_advice_details = true;

-- Prepared, but always_store_advice_details = true, so should show feedback.
PREPARE pt4 AS SELECT * FROM ptab;
EXPLAIN (COSTS OFF, PLAN_ADVICE) EXECUTE pt2;

