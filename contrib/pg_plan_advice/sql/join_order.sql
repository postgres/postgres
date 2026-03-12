LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 0;

CREATE TABLE jo_dim1 (id integer primary key, dim1 text, val1 int)
	WITH (autovacuum_enabled = false);
INSERT INTO jo_dim1 (id, dim1, val1)
	SELECT g, 'some filler text ' || g, (g % 3) + 1
	  FROM generate_series(1,100) g;
VACUUM ANALYZE jo_dim1;
CREATE TABLE jo_dim2 (id integer primary key, dim2 text, val2 int)
	WITH (autovacuum_enabled = false);
INSERT INTO jo_dim2 (id, dim2, val2)
	SELECT g, 'some filler text ' || g, (g % 53) + 1
	  FROM generate_series(1,1000) g;
VACUUM ANALYZE jo_dim2;

CREATE TABLE jo_fact (
	id int primary key,
	dim1_id integer not null references jo_dim1 (id),
	dim2_id integer not null references jo_dim2 (id)
) WITH (autovacuum_enabled = false);
INSERT INTO jo_fact
	SELECT g, (g%100)+1, (g%100)+1 FROM generate_series(1,100000) g;
VACUUM ANALYZE jo_fact;

-- We expect to join to d2 first and then d1, since the condition on d2
-- is more selective.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;

-- Force a few different join orders. Some of these are very inefficient,
-- but the planner considers them all viable.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'join_order(f d1 d2)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;
SET LOCAL pg_plan_advice.advice = 'join_order(f d2 d1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;
SET LOCAL pg_plan_advice.advice = 'join_order(d1 f d2)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;
SET LOCAL pg_plan_advice.advice = 'join_order(f (d1 d2))';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;
SET LOCAL pg_plan_advice.advice = 'join_order(f {d1 d2})';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;
COMMIT;

-- Force a join order by mentioning just a prefix of the join list.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'join_order(d2)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;
SET LOCAL pg_plan_advice.advice = 'join_order(d2 d1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;
COMMIT;

-- jo_fact is not partitioned, but let's try pretending that it is and
-- verifying that the advice does not apply.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'join_order(f/d1 d1 d2)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;
SET LOCAL pg_plan_advice.advice = 'join_order(f/d1 (d1 d2))';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_fact f
	LEFT JOIN jo_dim1 d1 ON f.dim1_id = d1.id
	LEFT JOIN jo_dim2 d2 ON f.dim2_id = d2.id
	WHERE val1 = 1 AND val2 = 1;
COMMIT;

-- The unusual formulation of this query is intended to prevent the query
-- planner from reducing the FULL JOIN to some other join type, so that we
-- can test what happens with a join type that cannot be reordered.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_dim1 d1
	INNER JOIN (jo_fact f FULL JOIN jo_dim2 d2 ON f.dim2_id + 0 = d2.id + 0)
	ON d1.id = f.dim1_id OR f.dim1_id IS NULL;

-- We should not be able to force the planner to join f to d1 first, because
-- that is not a valid join order, but we should be able to force the planner
-- to make either d2 or f the driving table.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'join_order(f d1 d2)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_dim1 d1
	INNER JOIN (jo_fact f FULL JOIN jo_dim2 d2 ON f.dim2_id + 0 = d2.id + 0)
	ON d1.id = f.dim1_id OR f.dim1_id IS NULL;
SET LOCAL pg_plan_advice.advice = 'join_order(f d2 d1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_dim1 d1
	INNER JOIN (jo_fact f FULL JOIN jo_dim2 d2 ON f.dim2_id + 0 = d2.id + 0)
	ON d1.id = f.dim1_id OR f.dim1_id IS NULL;
SET LOCAL pg_plan_advice.advice = 'join_order(d2 f d1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_dim1 d1
	INNER JOIN (jo_fact f FULL JOIN jo_dim2 d2 ON f.dim2_id + 0 = d2.id + 0)
	ON d1.id = f.dim1_id OR f.dim1_id IS NULL;
COMMIT;

-- Two incompatible join orders should conflict. In the second case,
-- the conflict is implicit: if d1 is on the inner side of a join of any
-- type, it cannot also be the driving table.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'join_order(f) join_order(d1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_dim1 d1
	INNER JOIN (jo_fact f FULL JOIN jo_dim2 d2 ON f.dim2_id + 0 = d2.id + 0)
	ON d1.id = f.dim1_id OR f.dim1_id IS NULL;
SET LOCAL pg_plan_advice.advice = 'join_order(d1) hash_join(d1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM jo_dim1 d1
	INNER JOIN (jo_fact f FULL JOIN jo_dim2 d2 ON f.dim2_id + 0 = d2.id + 0)
	ON d1.id = f.dim1_id OR f.dim1_id IS NULL;
COMMIT;
