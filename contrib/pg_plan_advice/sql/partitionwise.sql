LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 0;
SET enable_partitionwise_join = true;

CREATE TABLE pt1 (id integer primary key, dim1 text, val1 int)
	PARTITION BY RANGE (id);
CREATE TABLE pt1a PARTITION OF pt1 FOR VALUES FROM (1) to (1001)
	WITH (autovacuum_enabled = false);
CREATE TABLE pt1b PARTITION OF pt1 FOR VALUES FROM (1001) to (2001)
	WITH (autovacuum_enabled = false);
CREATE TABLE pt1c PARTITION OF pt1 FOR VALUES FROM (2001) to (3001)
	WITH (autovacuum_enabled = false);
INSERT INTO pt1 (id, dim1, val1)
	SELECT g, 'some filler text ' || g, (g % 3) + 1
	  FROM generate_series(1,3000) g;
VACUUM ANALYZE pt1;

CREATE TABLE pt2 (id integer primary key, dim2 text, val2 int)
	PARTITION BY RANGE (id);
CREATE TABLE pt2a PARTITION OF pt2 FOR VALUES FROM (1) to (1001)
	WITH (autovacuum_enabled = false);
CREATE TABLE pt2b PARTITION OF pt2 FOR VALUES FROM (1001) to (2001)
	WITH (autovacuum_enabled = false);
CREATE TABLE pt2c PARTITION OF pt2 FOR VALUES FROM (2001) to (3001)
	WITH (autovacuum_enabled = false);
INSERT INTO pt2 (id, dim2, val2)
	SELECT g, 'some other text ' || g, (g % 5) + 1
	  FROM generate_series(1,3000,2) g;
VACUUM ANALYZE pt2;

CREATE TABLE pt3 (id integer primary key, dim3 text, val3 int)
	PARTITION BY RANGE (id);
CREATE TABLE pt3a PARTITION OF pt3 FOR VALUES FROM (1) to (1001)
	WITH (autovacuum_enabled = false);
CREATE TABLE pt3b PARTITION OF pt3 FOR VALUES FROM (1001) to (2001)
	WITH (autovacuum_enabled = false);
CREATE TABLE pt3c PARTITION OF pt3 FOR VALUES FROM (2001) to (3001)
	WITH (autovacuum_enabled = false);
INSERT INTO pt3 (id, dim3, val3)
	SELECT g, 'a third random text ' || g, (g % 7) + 1
	  FROM generate_series(1,3000,3) g;
VACUUM ANALYZE pt3;

CREATE TABLE ptmismatch (id integer primary key, dimm text, valm int)
	PARTITION BY RANGE (id);
CREATE TABLE ptmismatcha PARTITION OF ptmismatch
    FOR VALUES FROM (1) to (1501)
	WITH (autovacuum_enabled = false);
CREATE TABLE ptmismatchb PARTITION OF ptmismatch
    FOR VALUES FROM (1501) to (3001)
	WITH (autovacuum_enabled = false);
INSERT INTO ptmismatch (id, dimm, valm)
	SELECT g, 'yet another text ' || g, (g % 2) + 1
	  FROM generate_series(1,3000) g;
VACUUM ANALYZE ptmismatch;

EXPLAIN (PLAN_ADVICE, COSTS OFF)
SELECT * FROM pt1, pt2, pt3 WHERE pt1.id = pt2.id AND pt2.id = pt3.id
   AND val1 = 1 AND val2 = 1 AND val3 = 1;

-- Suppress partitionwise join, or do it just partially.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'PARTITIONWISE(pt1 pt2 pt3)';
EXPLAIN (PLAN_ADVICE, COSTS OFF)
SELECT * FROM pt1, pt2, pt3 WHERE pt1.id = pt2.id AND pt2.id = pt3.id
   AND val1 = 1 AND val2 = 1 AND val3 = 1;
SET LOCAL pg_plan_advice.advice = 'PARTITIONWISE((pt1 pt2) pt3)';
EXPLAIN (PLAN_ADVICE, COSTS OFF)
SELECT * FROM pt1, pt2, pt3 WHERE pt1.id = pt2.id AND pt2.id = pt3.id
   AND val1 = 1 AND val2 = 1 AND val3 = 1;
COMMIT;

-- Test conflicting advice.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'PARTITIONWISE((pt1 pt2) (pt1 pt3))';
EXPLAIN (PLAN_ADVICE, COSTS OFF)
SELECT * FROM pt1, pt2, pt3 WHERE pt1.id = pt2.id AND pt2.id = pt3.id
   AND val1 = 1 AND val2 = 1 AND val3 = 1;
COMMIT;

-- Can't force a partitionwise join with a mismatched table.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'PARTITIONWISE((pt1 ptmismatch))';
EXPLAIN (PLAN_ADVICE, COSTS OFF)
SELECT * FROM pt1, ptmismatch WHERE pt1.id = ptmismatch.id;
COMMIT;

-- Force join order for a particular branch of the partitionwise join with
-- and without mentioning the schema name.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'JOIN_ORDER(pt3/public.pt3a pt2/public.pt2a pt1/public.pt1a)';
EXPLAIN (PLAN_ADVICE, COSTS OFF)
SELECT * FROM pt1, pt2, pt3 WHERE pt1.id = pt2.id AND pt2.id = pt3.id
   AND val1 = 1 AND val2 = 1 AND val3 = 1;
SET LOCAL pg_plan_advice.advice = 'JOIN_ORDER(pt3/pt3a pt2/pt2a pt1/pt1a)';
EXPLAIN (PLAN_ADVICE, COSTS OFF)
SELECT * FROM pt1, pt2, pt3 WHERE pt1.id = pt2.id AND pt2.id = pt3.id
   AND val1 = 1 AND val2 = 1 AND val3 = 1;
COMMIT;
