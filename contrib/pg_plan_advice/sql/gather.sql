LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 1;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET debug_parallel_query = off;

CREATE TABLE gt_dim (id serial primary key, dim text)
	WITH (autovacuum_enabled = false);
INSERT INTO gt_dim (dim) SELECT random()::text FROM generate_series(1,100) g;
VACUUM ANALYZE gt_dim;

CREATE TABLE gt_fact (
	id int not null,
	dim_id integer not null references gt_dim (id)
) WITH (autovacuum_enabled = false);
INSERT INTO gt_fact
	SELECT g, (g%3)+1 FROM generate_series(1,100000) g;
VACUUM ANALYZE gt_fact;

-- By default, we expect Gather Merge with a parallel hash join.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;

-- Force Gather or Gather Merge of both relations together.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'gather_merge((f d))';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
SET LOCAL pg_plan_advice.advice = 'gather((f d))';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
COMMIT;

-- Force a separate Gather or Gather Merge operation for each relation.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'gather_merge(f d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
SET LOCAL pg_plan_advice.advice = 'gather(f d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
SET LOCAL pg_plan_advice.advice = 'gather((d d/d.d))';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
COMMIT;

-- Force a Gather or Gather Merge on one relation but no parallelism on other.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'gather_merge(f) no_gather(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
SET LOCAL pg_plan_advice.advice = 'gather_merge(d) no_gather(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
SET LOCAL pg_plan_advice.advice = 'gather(f) no_gather(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
SET LOCAL pg_plan_advice.advice = 'gather(d) no_gather(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
COMMIT;

-- Force no Gather or Gather Merge use at all.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'no_gather(f d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
COMMIT;

-- Can't force Gather Merge without the ORDER BY clause, but just Gather is OK.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'gather_merge((f d))';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'gather((f d))';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id;
COMMIT;

-- Test conflicting advice.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'gather((f d)) no_gather(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM gt_fact f JOIN gt_dim d ON f.dim_id = d.id ORDER BY d.id;
COMMIT;
