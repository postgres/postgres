-- Generic extended statistics support

-- We will be checking execution plans without/with statistics, so
-- let's make sure we get simple non-parallel plans. Also set the
-- work_mem low so that we can use small amounts of data.
SET max_parallel_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET work_mem = '128kB';

-- Verify failures
CREATE STATISTICS tst;
CREATE STATISTICS tst ON a, b;
CREATE STATISTICS tst FROM sometab;
CREATE STATISTICS tst ON a, b FROM nonexistant;
CREATE STATISTICS tst ON a, b FROM pg_class;
CREATE STATISTICS tst ON relname, relname, relnatts FROM pg_class;
CREATE STATISTICS tst ON relnatts + relpages FROM pg_class;
CREATE STATISTICS tst ON (relpages, reltuples) FROM pg_class;
CREATE STATISTICS tst (unrecognized) ON relname, relnatts FROM pg_class;

-- Ensure stats are dropped sanely, and test IF NOT EXISTS while at it
CREATE TABLE ab1 (a INTEGER, b INTEGER, c INTEGER);
CREATE STATISTICS IF NOT EXISTS ab1_a_b_stats ON a, b FROM ab1;
CREATE STATISTICS IF NOT EXISTS ab1_a_b_stats ON a, b FROM ab1;
DROP STATISTICS ab1_a_b_stats;

CREATE SCHEMA regress_schema_2;
CREATE STATISTICS regress_schema_2.ab1_a_b_stats ON a, b FROM ab1;

-- Let's also verify the pg_get_statisticsobjdef output looks sane.
SELECT pg_get_statisticsobjdef(oid) FROM pg_statistic_ext WHERE stxname = 'ab1_a_b_stats';

DROP STATISTICS regress_schema_2.ab1_a_b_stats;

-- Ensure statistics are dropped when columns are
CREATE STATISTICS ab1_b_c_stats ON b, c FROM ab1;
CREATE STATISTICS ab1_a_b_c_stats ON a, b, c FROM ab1;
CREATE STATISTICS ab1_b_a_stats ON b, a FROM ab1;
ALTER TABLE ab1 DROP COLUMN a;
\d ab1
-- Ensure statistics are dropped when table is
SELECT stxname FROM pg_statistic_ext WHERE stxname LIKE 'ab1%';
DROP TABLE ab1;
SELECT stxname FROM pg_statistic_ext WHERE stxname LIKE 'ab1%';

-- Ensure things work sanely with SET STATISTICS 0
CREATE TABLE ab1 (a INTEGER, b INTEGER);
ALTER TABLE ab1 ALTER a SET STATISTICS 0;
INSERT INTO ab1 SELECT a, a%23 FROM generate_series(1, 1000) a;
CREATE STATISTICS ab1_a_b_stats ON a, b FROM ab1;
ANALYZE ab1;
ALTER TABLE ab1 ALTER a SET STATISTICS -1;
-- partial analyze doesn't build stats either
ANALYZE ab1 (a);
ANALYZE ab1;
DROP TABLE ab1;

-- Verify supported object types for extended statistics
CREATE schema tststats;

CREATE TABLE tststats.t (a int, b int, c text);
CREATE INDEX ti ON tststats.t (a, b);
CREATE SEQUENCE tststats.s;
CREATE VIEW tststats.v AS SELECT * FROM tststats.t;
CREATE MATERIALIZED VIEW tststats.mv AS SELECT * FROM tststats.t;
CREATE TYPE tststats.ty AS (a int, b int, c text);
CREATE FOREIGN DATA WRAPPER extstats_dummy_fdw;
CREATE SERVER extstats_dummy_srv FOREIGN DATA WRAPPER extstats_dummy_fdw;
CREATE FOREIGN TABLE tststats.f (a int, b int, c text) SERVER extstats_dummy_srv;
CREATE TABLE tststats.pt (a int, b int, c text) PARTITION BY RANGE (a, b);
CREATE TABLE tststats.pt1 PARTITION OF tststats.pt FOR VALUES FROM (-10, -10) TO (10, 10);

CREATE STATISTICS tststats.s1 ON a, b FROM tststats.t;
CREATE STATISTICS tststats.s2 ON a, b FROM tststats.ti;
CREATE STATISTICS tststats.s3 ON a, b FROM tststats.s;
CREATE STATISTICS tststats.s4 ON a, b FROM tststats.v;
CREATE STATISTICS tststats.s5 ON a, b FROM tststats.mv;
CREATE STATISTICS tststats.s6 ON a, b FROM tststats.ty;
CREATE STATISTICS tststats.s7 ON a, b FROM tststats.f;
CREATE STATISTICS tststats.s8 ON a, b FROM tststats.pt;
CREATE STATISTICS tststats.s9 ON a, b FROM tststats.pt1;
DO $$
DECLARE
	relname text := reltoastrelid::regclass FROM pg_class WHERE oid = 'tststats.t'::regclass;
BEGIN
	EXECUTE 'CREATE STATISTICS tststats.s10 ON a, b FROM ' || relname;
EXCEPTION WHEN wrong_object_type THEN
	RAISE NOTICE 'stats on toast table not created';
END;
$$;

\set VERBOSITY terse \\ -- suppress cascade details
DROP SCHEMA tststats CASCADE;
DROP FOREIGN DATA WRAPPER extstats_dummy_fdw CASCADE;
\set VERBOSITY default

-- n-distinct tests
CREATE TABLE ndistinct (
    filler1 TEXT,
    filler2 NUMERIC,
    a INT,
    b INT,
    filler3 DATE,
    c INT,
    d INT
);

-- over-estimates when using only per-column statistics
INSERT INTO ndistinct (a, b, c, filler1)
     SELECT i/100, i/100, i/100, cash_words((i/100)::money)
       FROM generate_series(1,30000) s(i);

ANALYZE ndistinct;

-- Group Aggregate, due to over-estimate of the number of groups
EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY b, c;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY b, c, d;

-- correct command
CREATE STATISTICS s10 ON a, b, c FROM ndistinct;

ANALYZE ndistinct;

SELECT stxkind, stxndistinct
  FROM pg_statistic_ext WHERE stxrelid = 'ndistinct'::regclass;

-- Hash Aggregate, thanks to estimates improved by the statistic
EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY b, c;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c;

-- last two plans keep using Group Aggregate, because 'd' is not covered
-- by the statistic and while it's NULL-only we assume 200 values for it
EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY b, c, d;

TRUNCATE TABLE ndistinct;

-- under-estimates when using only per-column statistics
INSERT INTO ndistinct (a, b, c, filler1)
     SELECT mod(i,50), mod(i,51), mod(i,32),
            cash_words(mod(i,33)::int::money)
       FROM generate_series(1,10000) s(i);

ANALYZE ndistinct;

SELECT stxkind, stxndistinct
  FROM pg_statistic_ext WHERE stxrelid = 'ndistinct'::regclass;

-- plans using Group Aggregate, thanks to using correct esimates
EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY b, c, d;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, d;

DROP STATISTICS s10;

SELECT stxkind, stxndistinct
  FROM pg_statistic_ext WHERE stxrelid = 'ndistinct'::regclass;

-- dropping the statistics switches the plans to Hash Aggregate,
-- due to under-estimates
EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY b, c, d;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, d;

-- functional dependencies tests
CREATE TABLE functional_dependencies (
    filler1 TEXT,
    filler2 NUMERIC,
    a INT,
    b TEXT,
    filler3 DATE,
    c INT,
    d TEXT
);

SET random_page_cost = 1.2;

CREATE INDEX fdeps_ab_idx ON functional_dependencies (a, b);
CREATE INDEX fdeps_abc_idx ON functional_dependencies (a, b, c);

-- random data (no functional dependencies)
INSERT INTO functional_dependencies (a, b, c, filler1)
     SELECT mod(i, 23), mod(i, 29), mod(i, 31), i FROM generate_series(1,5000) s(i);

ANALYZE functional_dependencies;

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1';

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1' AND c = 1;

-- create statistics
CREATE STATISTICS func_deps_stat (dependencies) ON a, b, c FROM functional_dependencies;

ANALYZE functional_dependencies;

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1';

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1' AND c = 1;

-- a => b, a => c, b => c
TRUNCATE functional_dependencies;
DROP STATISTICS func_deps_stat;

INSERT INTO functional_dependencies (a, b, c, filler1)
     SELECT mod(i,100), mod(i,50), mod(i,25), i FROM generate_series(1,5000) s(i);

ANALYZE functional_dependencies;

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1';

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1' AND c = 1;

-- create statistics
CREATE STATISTICS func_deps_stat (dependencies) ON a, b, c FROM functional_dependencies;

ANALYZE functional_dependencies;

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1';

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1' AND c = 1;

-- check change of column type doesn't break it
ALTER TABLE functional_dependencies ALTER COLUMN c TYPE numeric;

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1' AND c = 1;

ANALYZE functional_dependencies;

EXPLAIN (COSTS OFF)
 SELECT * FROM functional_dependencies WHERE a = 1 AND b = '1' AND c = 1;

RESET random_page_cost;
