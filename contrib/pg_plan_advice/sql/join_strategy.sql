LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 0;

CREATE TABLE join_dim (id serial primary key, dim text)
	WITH (autovacuum_enabled = false);
INSERT INTO join_dim (dim) SELECT random()::text FROM generate_series(1,100) g;
VACUUM ANALYZE join_dim;

CREATE TABLE join_fact (
	id int primary key,
	dim_id integer not null references join_dim (id)
) WITH (autovacuum_enabled = false);
INSERT INTO join_fact
	SELECT g, (g%3)+1 FROM generate_series(1,100000) g;
CREATE INDEX join_fact_dim_id ON join_fact (dim_id);
VACUUM ANALYZE join_fact;

-- We expect a hash join by default.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;

-- Try forcing each join method in turn with join_dim as the inner table.
-- All of these should work except for MERGE_JOIN_MATERIALIZE; that will
-- fail, because the planner knows that join_dim (id) is unique, and will
-- refuse to add mark/restore overhead.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'HASH_JOIN(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'MERGE_JOIN_MATERIALIZE(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'MERGE_JOIN_PLAIN(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NESTED_LOOP_MATERIALIZE(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NESTED_LOOP_MEMOIZE(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NESTED_LOOP_PLAIN(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
COMMIT;

-- Now try forcing each join method in turn with join_fact as the inner
-- table. All of these should work.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'HASH_JOIN(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'MERGE_JOIN_MATERIALIZE(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'MERGE_JOIN_PLAIN(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NESTED_LOOP_MATERIALIZE(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NESTED_LOOP_MEMOIZE(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NESTED_LOOP_PLAIN(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
COMMIT;

-- Non-working cases. We can't force a foreign join between these tables,
-- because they aren't foreign tables. We also can't use two different
-- strategies on the same table, nor can we put both tables on the inner
-- side of the same join.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'FOREIGN_JOIN((f d))';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NESTED_LOOP_PLAIN(f) NESTED_LOOP_MATERIALIZE(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NESTED_LOOP_PLAIN(f d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM join_fact f JOIN join_dim d ON f.dim_id = d.id;
COMMIT;
