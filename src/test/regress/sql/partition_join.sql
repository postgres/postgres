--
-- PARTITION_JOIN
-- Test partitionwise join between partitioned tables
--

-- Enable partitionwise join, which by default is disabled.
SET enable_partitionwise_join to true;

--
-- partitioned by a single column
--
CREATE TABLE prt1 (a int, b int, c varchar) PARTITION BY RANGE(a);
CREATE TABLE prt1_p1 PARTITION OF prt1 FOR VALUES FROM (0) TO (250);
CREATE TABLE prt1_p3 PARTITION OF prt1 FOR VALUES FROM (500) TO (600);
CREATE TABLE prt1_p2 PARTITION OF prt1 FOR VALUES FROM (250) TO (500);
INSERT INTO prt1 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 2 = 0;
CREATE INDEX iprt1_p1_a on prt1_p1(a);
CREATE INDEX iprt1_p2_a on prt1_p2(a);
CREATE INDEX iprt1_p3_a on prt1_p3(a);
ANALYZE prt1;

CREATE TABLE prt2 (a int, b int, c varchar) PARTITION BY RANGE(b);
CREATE TABLE prt2_p1 PARTITION OF prt2 FOR VALUES FROM (0) TO (250);
CREATE TABLE prt2_p2 PARTITION OF prt2 FOR VALUES FROM (250) TO (500);
CREATE TABLE prt2_p3 PARTITION OF prt2 FOR VALUES FROM (500) TO (600);
INSERT INTO prt2 SELECT i % 25, i, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 3 = 0;
CREATE INDEX iprt2_p1_b on prt2_p1(b);
CREATE INDEX iprt2_p2_b on prt2_p2(b);
CREATE INDEX iprt2_p3_b on prt2_p3(b);
ANALYZE prt2;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;

-- left outer join, with whole-row reference; partitionwise join does not apply
EXPLAIN (COSTS OFF)
SELECT t1, t2 FROM prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1, t2 FROM prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- right outer join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 RIGHT JOIN prt2 t2 ON t1.a = t2.b WHERE t2.a = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 RIGHT JOIN prt2 t2 ON t1.a = t2.b WHERE t2.a = 0 ORDER BY t1.a, t2.b;

-- full outer join, with placeholder vars
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT 50 phv, * FROM prt1 WHERE prt1.b = 0) t1 FULL JOIN (SELECT 75 phv, * FROM prt2 WHERE prt2.a = 0) t2 ON (t1.a = t2.b) WHERE t1.phv = t1.a OR t2.phv = t2.b ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT 50 phv, * FROM prt1 WHERE prt1.b = 0) t1 FULL JOIN (SELECT 75 phv, * FROM prt2 WHERE prt2.a = 0) t2 ON (t1.a = t2.b) WHERE t1.phv = t1.a OR t2.phv = t2.b ORDER BY t1.a, t2.b;

-- Join with pruned partitions from joining relations
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.a < 450 AND t2.b > 250 AND t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.a < 450 AND t2.b > 250 AND t1.b = 0 ORDER BY t1.a, t2.b;

-- Currently we can't do partitioned join if nullable-side partitions are pruned
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- Currently we can't do partitioned join if nullable-side partitions are pruned
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 FULL JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 OR t2.a = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 FULL JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 OR t2.a = 0 ORDER BY t1.a, t2.b;

-- Semi-join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t2.b FROM prt2 t2 WHERE t2.a = 0) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t2.b FROM prt2 t2 WHERE t2.a = 0) AND t1.b = 0 ORDER BY t1.a;

-- Anti-join with aggregates
EXPLAIN (COSTS OFF)
SELECT sum(t1.a), avg(t1.a), sum(t1.b), avg(t1.b) FROM prt1 t1 WHERE NOT EXISTS (SELECT 1 FROM prt2 t2 WHERE t1.a = t2.b);
SELECT sum(t1.a), avg(t1.a), sum(t1.b), avg(t1.b) FROM prt1 t1 WHERE NOT EXISTS (SELECT 1 FROM prt2 t2 WHERE t1.a = t2.b);

-- lateral reference
EXPLAIN (COSTS OFF)
SELECT * FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t3.a AS t3a, least(t1.a,t2.a,t3.b) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss
			  ON t1.a = ss.t2a WHERE t1.b = 0 ORDER BY t1.a;
SELECT * FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t3.a AS t3a, least(t1.a,t2.a,t3.b) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss
			  ON t1.a = ss.t2a WHERE t1.b = 0 ORDER BY t1.a;

EXPLAIN (COSTS OFF)
SELECT t1.a, ss.t2a, ss.t2c FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t3.a AS t3a, t2.b t2b, t2.c t2c, least(t1.a,t2.a,t3.b) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss
			  ON t1.c = ss.t2c WHERE (t1.b + coalesce(ss.t2b, 0)) = 0 ORDER BY t1.a;
SELECT t1.a, ss.t2a, ss.t2c FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t3.a AS t3a, t2.b t2b, t2.c t2c, least(t1.a,t2.a,t3.a) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss
			  ON t1.c = ss.t2c WHERE (t1.b + coalesce(ss.t2b, 0)) = 0 ORDER BY t1.a;

SET max_parallel_workers_per_gather = 0;
-- If there are lateral references to the other relation in sample scan,
-- we cannot generate a partitionwise join.
EXPLAIN (COSTS OFF)
SELECT * FROM prt1 t1 JOIN LATERAL
			  (SELECT * FROM prt1 t2 TABLESAMPLE SYSTEM (t1.a) REPEATABLE(t1.b)) s
			  ON t1.a = s.a;

-- If there are lateral references to the other relation in scan's restriction
-- clauses, we cannot generate a partitionwise join.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t1.b AS t1b, t2.* FROM prt2 t2) s
			  ON t1.a = s.b WHERE s.t1b = s.a;
SELECT count(*) FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t1.b AS t1b, t2.* FROM prt2 t2) s
			  ON t1.a = s.b WHERE s.t1b = s.a;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t1.b AS t1b, t2.* FROM prt2 t2) s
			  ON t1.a = s.b WHERE s.t1b = s.b;
SELECT count(*) FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t1.b AS t1b, t2.* FROM prt2 t2) s
			  ON t1.a = s.b WHERE s.t1b = s.b;
RESET max_parallel_workers_per_gather;

-- bug with inadequate sort key representation
SET enable_partitionwise_aggregate TO true;
SET enable_hashjoin TO false;

EXPLAIN (COSTS OFF)
SELECT a, b FROM prt1 FULL JOIN prt2 p2(b,a,c) USING(a,b)
  WHERE a BETWEEN 490 AND 510
  GROUP BY 1, 2 ORDER BY 1, 2;
SELECT a, b FROM prt1 FULL JOIN prt2 p2(b,a,c) USING(a,b)
  WHERE a BETWEEN 490 AND 510
  GROUP BY 1, 2 ORDER BY 1, 2;

RESET enable_partitionwise_aggregate;
RESET enable_hashjoin;

--
-- partitioned by expression
--
CREATE TABLE prt1_e (a int, b int, c int) PARTITION BY RANGE(((a + b)/2));
CREATE TABLE prt1_e_p1 PARTITION OF prt1_e FOR VALUES FROM (0) TO (250);
CREATE TABLE prt1_e_p2 PARTITION OF prt1_e FOR VALUES FROM (250) TO (500);
CREATE TABLE prt1_e_p3 PARTITION OF prt1_e FOR VALUES FROM (500) TO (600);
INSERT INTO prt1_e SELECT i, i, i % 25 FROM generate_series(0, 599, 2) i;
CREATE INDEX iprt1_e_p1_ab2 on prt1_e_p1(((a+b)/2));
CREATE INDEX iprt1_e_p2_ab2 on prt1_e_p2(((a+b)/2));
CREATE INDEX iprt1_e_p3_ab2 on prt1_e_p3(((a+b)/2));
ANALYZE prt1_e;

CREATE TABLE prt2_e (a int, b int, c int) PARTITION BY RANGE(((b + a)/2));
CREATE TABLE prt2_e_p1 PARTITION OF prt2_e FOR VALUES FROM (0) TO (250);
CREATE TABLE prt2_e_p2 PARTITION OF prt2_e FOR VALUES FROM (250) TO (500);
CREATE TABLE prt2_e_p3 PARTITION OF prt2_e FOR VALUES FROM (500) TO (600);
INSERT INTO prt2_e SELECT i, i, i % 25 FROM generate_series(0, 599, 3) i;
ANALYZE prt2_e;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_e t1, prt2_e t2 WHERE (t1.a + t1.b)/2 = (t2.b + t2.a)/2 AND t1.c = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_e t1, prt2_e t2 WHERE (t1.a + t1.b)/2 = (t2.b + t2.a)/2 AND t1.c = 0 ORDER BY t1.a, t2.b;

--
-- N-way join
--
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM prt1 t1, prt2 t2, prt1_e t3 WHERE t1.a = t2.b AND t1.a = (t3.a + t3.b)/2 AND t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM prt1 t1, prt2 t2, prt1_e t3 WHERE t1.a = t2.b AND t1.a = (t3.a + t3.b)/2 AND t1.b = 0 ORDER BY t1.a, t2.b;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) LEFT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t1.b = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) LEFT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t1.b = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) RIGHT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t3.c = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) RIGHT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t3.c = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;

-- Cases with non-nullable expressions in subquery results;
-- make sure these go to null as expected
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.phv, t2.b, t2.phv, t3.a + t3.b, t3.phv FROM ((SELECT 50 phv, * FROM prt1 WHERE prt1.b = 0) t1 FULL JOIN (SELECT 75 phv, * FROM prt2 WHERE prt2.a = 0) t2 ON (t1.a = t2.b)) FULL JOIN (SELECT 50 phv, * FROM prt1_e WHERE prt1_e.c = 0) t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t1.a = t1.phv OR t2.b = t2.phv OR (t3.a + t3.b)/2 = t3.phv ORDER BY t1.a, t2.b, t3.a + t3.b;
SELECT t1.a, t1.phv, t2.b, t2.phv, t3.a + t3.b, t3.phv FROM ((SELECT 50 phv, * FROM prt1 WHERE prt1.b = 0) t1 FULL JOIN (SELECT 75 phv, * FROM prt2 WHERE prt2.a = 0) t2 ON (t1.a = t2.b)) FULL JOIN (SELECT 50 phv, * FROM prt1_e WHERE prt1_e.c = 0) t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t1.a = t1.phv OR t2.b = t2.phv OR (t3.a + t3.b)/2 = t3.phv ORDER BY t1.a, t2.b, t3.a + t3.b;

-- Semi-join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1, prt1_e t2 WHERE t1.a = 0 AND t1.b = (t2.a + t2.b)/2) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1, prt1_e t2 WHERE t1.a = 0 AND t1.b = (t2.a + t2.b)/2) AND t1.b = 0 ORDER BY t1.a;

EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1 WHERE t1.b IN (SELECT (t1.a + t1.b)/2 FROM prt1_e t1 WHERE t1.c = 0)) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1 WHERE t1.b IN (SELECT (t1.a + t1.b)/2 FROM prt1_e t1 WHERE t1.c = 0)) AND t1.b = 0 ORDER BY t1.a;

-- test merge joins
SET enable_hashjoin TO off;
SET enable_nestloop TO off;

EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1 WHERE t1.b IN (SELECT (t1.a + t1.b)/2 FROM prt1_e t1 WHERE t1.c = 0)) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1 WHERE t1.b IN (SELECT (t1.a + t1.b)/2 FROM prt1_e t1 WHERE t1.c = 0)) AND t1.b = 0 ORDER BY t1.a;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) RIGHT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t3.c = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) RIGHT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t3.c = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;

-- MergeAppend on nullable column
-- This should generate a partitionwise join, but currently fails to
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.b FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t2.b FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- merge join when expression with whole-row reference needs to be sorted;
-- partitionwise join does not apply
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.b FROM prt1 t1, prt2 t2 WHERE t1::text = t2::text AND t1.a = t2.b ORDER BY t1.a;
SELECT t1.a, t2.b FROM prt1 t1, prt2 t2 WHERE t1::text = t2::text AND t1.a = t2.b ORDER BY t1.a;

RESET enable_hashjoin;
RESET enable_nestloop;

--
-- partitioned by multiple columns
--
CREATE TABLE prt1_m (a int, b int, c int) PARTITION BY RANGE(a, ((a + b)/2));
CREATE TABLE prt1_m_p1 PARTITION OF prt1_m FOR VALUES FROM (0, 0) TO (250, 250);
CREATE TABLE prt1_m_p2 PARTITION OF prt1_m FOR VALUES FROM (250, 250) TO (500, 500);
CREATE TABLE prt1_m_p3 PARTITION OF prt1_m FOR VALUES FROM (500, 500) TO (600, 600);
INSERT INTO prt1_m SELECT i, i, i % 25 FROM generate_series(0, 599, 2) i;
ANALYZE prt1_m;

CREATE TABLE prt2_m (a int, b int, c int) PARTITION BY RANGE(((b + a)/2), b);
CREATE TABLE prt2_m_p1 PARTITION OF prt2_m FOR VALUES FROM (0, 0) TO (250, 250);
CREATE TABLE prt2_m_p2 PARTITION OF prt2_m FOR VALUES FROM (250, 250) TO (500, 500);
CREATE TABLE prt2_m_p3 PARTITION OF prt2_m FOR VALUES FROM (500, 500) TO (600, 600);
INSERT INTO prt2_m SELECT i, i, i % 25 FROM generate_series(0, 599, 3) i;
ANALYZE prt2_m;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_m WHERE prt1_m.c = 0) t1 FULL JOIN (SELECT * FROM prt2_m WHERE prt2_m.c = 0) t2 ON (t1.a = (t2.b + t2.a)/2 AND t2.b = (t1.a + t1.b)/2) ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_m WHERE prt1_m.c = 0) t1 FULL JOIN (SELECT * FROM prt2_m WHERE prt2_m.c = 0) t2 ON (t1.a = (t2.b + t2.a)/2 AND t2.b = (t1.a + t1.b)/2) ORDER BY t1.a, t2.b;

--
-- tests for list partitioned tables.
--
CREATE TABLE plt1 (a int, b int, c text) PARTITION BY LIST(c);
CREATE TABLE plt1_p1 PARTITION OF plt1 FOR VALUES IN ('0000', '0003', '0004', '0010');
CREATE TABLE plt1_p2 PARTITION OF plt1 FOR VALUES IN ('0001', '0005', '0002', '0009');
CREATE TABLE plt1_p3 PARTITION OF plt1 FOR VALUES IN ('0006', '0007', '0008', '0011');
INSERT INTO plt1 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE plt1;

CREATE TABLE plt2 (a int, b int, c text) PARTITION BY LIST(c);
CREATE TABLE plt2_p1 PARTITION OF plt2 FOR VALUES IN ('0000', '0003', '0004', '0010');
CREATE TABLE plt2_p2 PARTITION OF plt2 FOR VALUES IN ('0001', '0005', '0002', '0009');
CREATE TABLE plt2_p3 PARTITION OF plt2 FOR VALUES IN ('0006', '0007', '0008', '0011');
INSERT INTO plt2 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 3) i;
ANALYZE plt2;

--
-- list partitioned by expression
--
CREATE TABLE plt1_e (a int, b int, c text) PARTITION BY LIST(ltrim(c, 'A'));
CREATE TABLE plt1_e_p1 PARTITION OF plt1_e FOR VALUES IN ('0000', '0003', '0004', '0010');
CREATE TABLE plt1_e_p2 PARTITION OF plt1_e FOR VALUES IN ('0001', '0005', '0002', '0009');
CREATE TABLE plt1_e_p3 PARTITION OF plt1_e FOR VALUES IN ('0006', '0007', '0008', '0011');
INSERT INTO plt1_e SELECT i, i, 'A' || to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE plt1_e;

-- test partition matching with N-way join
EXPLAIN (COSTS OFF)
SELECT avg(t1.a), avg(t2.b), avg(t3.a + t3.b), t1.c, t2.c, t3.c FROM plt1 t1, plt2 t2, plt1_e t3 WHERE t1.b = t2.b AND t1.c = t2.c AND ltrim(t3.c, 'A') = t1.c GROUP BY t1.c, t2.c, t3.c ORDER BY t1.c, t2.c, t3.c;
SELECT avg(t1.a), avg(t2.b), avg(t3.a + t3.b), t1.c, t2.c, t3.c FROM plt1 t1, plt2 t2, plt1_e t3 WHERE t1.b = t2.b AND t1.c = t2.c AND ltrim(t3.c, 'A') = t1.c GROUP BY t1.c, t2.c, t3.c ORDER BY t1.c, t2.c, t3.c;

-- joins where one of the relations is proven empty
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.a = 1 AND t1.a = 2;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a = 1 AND a = 2) t1 LEFT JOIN prt2 t2 ON t1.a = t2.b;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a = 1 AND a = 2) t1 RIGHT JOIN prt2 t2 ON t1.a = t2.b, prt1 t3 WHERE t2.b = t3.a;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a = 1 AND a = 2) t1 FULL JOIN prt2 t2 ON t1.a = t2.b WHERE t2.a = 0 ORDER BY t1.a, t2.b;

--
-- tests for hash partitioned tables.
--
CREATE TABLE pht1 (a int, b int, c text) PARTITION BY HASH(c);
CREATE TABLE pht1_p1 PARTITION OF pht1 FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE pht1_p2 PARTITION OF pht1 FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE pht1_p3 PARTITION OF pht1 FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO pht1 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE pht1;

CREATE TABLE pht2 (a int, b int, c text) PARTITION BY HASH(c);
CREATE TABLE pht2_p1 PARTITION OF pht2 FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE pht2_p2 PARTITION OF pht2 FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE pht2_p3 PARTITION OF pht2 FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO pht2 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 3) i;
ANALYZE pht2;

--
-- hash partitioned by expression
--
CREATE TABLE pht1_e (a int, b int, c text) PARTITION BY HASH(ltrim(c, 'A'));
CREATE TABLE pht1_e_p1 PARTITION OF pht1_e FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE pht1_e_p2 PARTITION OF pht1_e FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE pht1_e_p3 PARTITION OF pht1_e FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO pht1_e SELECT i, i, 'A' || to_char(i/50, 'FM0000') FROM generate_series(0, 299, 2) i;
ANALYZE pht1_e;

-- test partition matching with N-way join
EXPLAIN (COSTS OFF)
SELECT avg(t1.a), avg(t2.b), avg(t3.a + t3.b), t1.c, t2.c, t3.c FROM pht1 t1, pht2 t2, pht1_e t3 WHERE t1.b = t2.b AND t1.c = t2.c AND ltrim(t3.c, 'A') = t1.c GROUP BY t1.c, t2.c, t3.c ORDER BY t1.c, t2.c, t3.c;
SELECT avg(t1.a), avg(t2.b), avg(t3.a + t3.b), t1.c, t2.c, t3.c FROM pht1 t1, pht2 t2, pht1_e t3 WHERE t1.b = t2.b AND t1.c = t2.c AND ltrim(t3.c, 'A') = t1.c GROUP BY t1.c, t2.c, t3.c ORDER BY t1.c, t2.c, t3.c;

-- test default partition behavior for range
ALTER TABLE prt1 DETACH PARTITION prt1_p3;
ALTER TABLE prt1 ATTACH PARTITION prt1_p3 DEFAULT;
ANALYZE prt1;
ALTER TABLE prt2 DETACH PARTITION prt2_p3;
ALTER TABLE prt2 ATTACH PARTITION prt2_p3 DEFAULT;
ANALYZE prt2;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;

-- test default partition behavior for list
ALTER TABLE plt1 DETACH PARTITION plt1_p3;
ALTER TABLE plt1 ATTACH PARTITION plt1_p3 DEFAULT;
ANALYZE plt1;
ALTER TABLE plt2 DETACH PARTITION plt2_p3;
ALTER TABLE plt2 ATTACH PARTITION plt2_p3 DEFAULT;
ANALYZE plt2;

EXPLAIN (COSTS OFF)
SELECT avg(t1.a), avg(t2.b), t1.c, t2.c FROM plt1 t1 RIGHT JOIN plt2 t2 ON t1.c = t2.c WHERE t1.a % 25 = 0 GROUP BY t1.c, t2.c ORDER BY t1.c, t2.c;
--
-- multiple levels of partitioning
--
CREATE TABLE prt1_l (a int, b int, c varchar) PARTITION BY RANGE(a);
CREATE TABLE prt1_l_p1 PARTITION OF prt1_l FOR VALUES FROM (0) TO (250);
CREATE TABLE prt1_l_p2 PARTITION OF prt1_l FOR VALUES FROM (250) TO (500) PARTITION BY LIST (c);
CREATE TABLE prt1_l_p2_p1 PARTITION OF prt1_l_p2 FOR VALUES IN ('0000', '0001');
CREATE TABLE prt1_l_p2_p2 PARTITION OF prt1_l_p2 FOR VALUES IN ('0002', '0003');
CREATE TABLE prt1_l_p3 PARTITION OF prt1_l FOR VALUES FROM (500) TO (600) PARTITION BY RANGE (b);
CREATE TABLE prt1_l_p3_p1 PARTITION OF prt1_l_p3 FOR VALUES FROM (0) TO (13);
CREATE TABLE prt1_l_p3_p2 PARTITION OF prt1_l_p3 FOR VALUES FROM (13) TO (25);
INSERT INTO prt1_l SELECT i, i % 25, to_char(i % 4, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE prt1_l;

CREATE TABLE prt2_l (a int, b int, c varchar) PARTITION BY RANGE(b);
CREATE TABLE prt2_l_p1 PARTITION OF prt2_l FOR VALUES FROM (0) TO (250);
CREATE TABLE prt2_l_p2 PARTITION OF prt2_l FOR VALUES FROM (250) TO (500) PARTITION BY LIST (c);
CREATE TABLE prt2_l_p2_p1 PARTITION OF prt2_l_p2 FOR VALUES IN ('0000', '0001');
CREATE TABLE prt2_l_p2_p2 PARTITION OF prt2_l_p2 FOR VALUES IN ('0002', '0003');
CREATE TABLE prt2_l_p3 PARTITION OF prt2_l FOR VALUES FROM (500) TO (600) PARTITION BY RANGE (a);
CREATE TABLE prt2_l_p3_p1 PARTITION OF prt2_l_p3 FOR VALUES FROM (0) TO (13);
CREATE TABLE prt2_l_p3_p2 PARTITION OF prt2_l_p3 FOR VALUES FROM (13) TO (25);
INSERT INTO prt2_l SELECT i % 25, i, to_char(i % 4, 'FM0000') FROM generate_series(0, 599, 3) i;
ANALYZE prt2_l;

-- inner join, qual covering only top-level partitions
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1, prt2_l t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1, prt2_l t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1 LEFT JOIN prt2_l t2 ON t1.a = t2.b AND t1.c = t2.c WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1 LEFT JOIN prt2_l t2 ON t1.a = t2.b AND t1.c = t2.c WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- right join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1 RIGHT JOIN prt2_l t2 ON t1.a = t2.b AND t1.c = t2.c WHERE t2.a = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1 RIGHT JOIN prt2_l t2 ON t1.a = t2.b AND t1.c = t2.c WHERE t2.a = 0 ORDER BY t1.a, t2.b;

-- full join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_l WHERE prt1_l.b = 0) t1 FULL JOIN (SELECT * FROM prt2_l WHERE prt2_l.a = 0) t2 ON (t1.a = t2.b AND t1.c = t2.c) ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_l WHERE prt1_l.b = 0) t1 FULL JOIN (SELECT * FROM prt2_l WHERE prt2_l.a = 0) t2 ON (t1.a = t2.b AND t1.c = t2.c) ORDER BY t1.a, t2.b;

-- lateral partitionwise join
EXPLAIN (COSTS OFF)
SELECT * FROM prt1_l t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t2.c AS t2c, t2.b AS t2b, t3.b AS t3b, least(t1.a,t2.a,t3.b) FROM prt1_l t2 JOIN prt2_l t3 ON (t2.a = t3.b AND t2.c = t3.c)) ss
			  ON t1.a = ss.t2a AND t1.c = ss.t2c WHERE t1.b = 0 ORDER BY t1.a;
SELECT * FROM prt1_l t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t2.c AS t2c, t2.b AS t2b, t3.b AS t3b, least(t1.a,t2.a,t3.b) FROM prt1_l t2 JOIN prt2_l t3 ON (t2.a = t3.b AND t2.c = t3.c)) ss
			  ON t1.a = ss.t2a AND t1.c = ss.t2c WHERE t1.b = 0 ORDER BY t1.a;

SET max_parallel_workers_per_gather = 0;
-- If there are lateral references to the other relation in sample scan,
-- we cannot generate a partitionwise join.
EXPLAIN (COSTS OFF)
SELECT * FROM prt1_l t1 JOIN LATERAL
			  (SELECT * FROM prt1_l t2 TABLESAMPLE SYSTEM (t1.a) REPEATABLE(t1.b)) s
			  ON t1.a = s.a AND t1.b = s.b AND t1.c = s.c;

-- If there are lateral references to the other relation in scan's restriction
-- clauses, we cannot generate a partitionwise join.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) FROM prt1_l t1 LEFT JOIN LATERAL
			  (SELECT t1.b AS t1b, t2.* FROM prt2_l t2) s
			  ON t1.a = s.b AND t1.b = s.a AND t1.c = s.c
			  WHERE s.t1b = s.a;
SELECT COUNT(*) FROM prt1_l t1 LEFT JOIN LATERAL
			  (SELECT t1.b AS t1b, t2.* FROM prt2_l t2) s
			  ON t1.a = s.b AND t1.b = s.a AND t1.c = s.c
			  WHERE s.t1b = s.a;
RESET max_parallel_workers_per_gather;

-- join with one side empty
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_l WHERE a = 1 AND a = 2) t1 RIGHT JOIN prt2_l t2 ON t1.a = t2.b AND t1.b = t2.a AND t1.c = t2.c;

-- Test case to verify proper handling of subqueries in a partitioned delete.
-- The weird-looking lateral join is just there to force creation of a
-- nestloop parameter within the subquery, which exposes the problem if the
-- planner fails to make multiple copies of the subquery as appropriate.
EXPLAIN (COSTS OFF)
DELETE FROM prt1_l
WHERE EXISTS (
  SELECT 1
    FROM int4_tbl,
         LATERAL (SELECT int4_tbl.f1 FROM int8_tbl LIMIT 2) ss
    WHERE prt1_l.c IS NULL);

--
-- negative testcases
--
CREATE TABLE prt1_n (a int, b int, c varchar) PARTITION BY RANGE(c);
CREATE TABLE prt1_n_p1 PARTITION OF prt1_n FOR VALUES FROM ('0000') TO ('0250');
CREATE TABLE prt1_n_p2 PARTITION OF prt1_n FOR VALUES FROM ('0250') TO ('0500');
INSERT INTO prt1_n SELECT i, i, to_char(i, 'FM0000') FROM generate_series(0, 499, 2) i;
ANALYZE prt1_n;

CREATE TABLE prt2_n (a int, b int, c text) PARTITION BY LIST(c);
CREATE TABLE prt2_n_p1 PARTITION OF prt2_n FOR VALUES IN ('0000', '0003', '0004', '0010', '0006', '0007');
CREATE TABLE prt2_n_p2 PARTITION OF prt2_n FOR VALUES IN ('0001', '0005', '0002', '0009', '0008', '0011');
INSERT INTO prt2_n SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE prt2_n;

CREATE TABLE prt3_n (a int, b int, c text) PARTITION BY LIST(c);
CREATE TABLE prt3_n_p1 PARTITION OF prt3_n FOR VALUES IN ('0000', '0004', '0006', '0007');
CREATE TABLE prt3_n_p2 PARTITION OF prt3_n FOR VALUES IN ('0001', '0002', '0008', '0010');
CREATE TABLE prt3_n_p3 PARTITION OF prt3_n FOR VALUES IN ('0003', '0005', '0009', '0011');
INSERT INTO prt2_n SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE prt3_n;

CREATE TABLE prt4_n (a int, b int, c text) PARTITION BY RANGE(a);
CREATE TABLE prt4_n_p1 PARTITION OF prt4_n FOR VALUES FROM (0) TO (300);
CREATE TABLE prt4_n_p2 PARTITION OF prt4_n FOR VALUES FROM (300) TO (500);
CREATE TABLE prt4_n_p3 PARTITION OF prt4_n FOR VALUES FROM (500) TO (600);
INSERT INTO prt4_n SELECT i, i, to_char(i, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE prt4_n;

-- partitionwise join can not be applied if the partition ranges differ
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt4_n t2 WHERE t1.a = t2.a;
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt4_n t2, prt2 t3 WHERE t1.a = t2.a and t1.a = t3.b;

-- partitionwise join can not be applied if there are no equi-join conditions
-- between partition keys
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 LEFT JOIN prt2 t2 ON (t1.a < t2.b);

-- equi-join with join condition on partial keys does not qualify for
-- partitionwise join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_m t1, prt2_m t2 WHERE t1.a = (t2.b + t2.a)/2;

-- equi-join between out-of-order partition key columns does not qualify for
-- partitionwise join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_m t1 LEFT JOIN prt2_m t2 ON t1.a = t2.b;

-- equi-join between non-key columns does not qualify for partitionwise join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_m t1 LEFT JOIN prt2_m t2 ON t1.c = t2.c;

-- partitionwise join can not be applied for a join between list and range
-- partitioned tables
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_n t1 LEFT JOIN prt2_n t2 ON (t1.c = t2.c);

-- partitionwise join can not be applied between tables with different
-- partition lists
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_n t1 JOIN prt2_n t2 ON (t1.c = t2.c) JOIN plt1 t3 ON (t1.c = t3.c);

-- partitionwise join can not be applied for a join between key column and
-- non-key column
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_n t1 FULL JOIN prt1 t2 ON (t1.c = t2.c);

-- partitionwise join can not be applied if only one of joining tables has
-- default partition
ALTER TABLE prt2 DETACH PARTITION prt2_p3;
ALTER TABLE prt2 ATTACH PARTITION prt2_p3 FOR VALUES FROM (500) TO (600);
ANALYZE prt2;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;

--
-- Test some other plan types in a partitionwise join (unfortunately,
-- we need larger tables to get the planner to choose these plan types)
--
create temp table prtx1 (a integer, b integer, c integer)
  partition by range (a);
create temp table prtx1_1 partition of prtx1 for values from (1) to (11);
create temp table prtx1_2 partition of prtx1 for values from (11) to (21);
create temp table prtx1_3 partition of prtx1 for values from (21) to (31);
create temp table prtx2 (a integer, b integer, c integer)
  partition by range (a);
create temp table prtx2_1 partition of prtx2 for values from (1) to (11);
create temp table prtx2_2 partition of prtx2 for values from (11) to (21);
create temp table prtx2_3 partition of prtx2 for values from (21) to (31);
insert into prtx1 select 1 + i%30, i, i
  from generate_series(1,1000) i;
insert into prtx2 select 1 + i%30, i, i
  from generate_series(1,500) i, generate_series(1,10) j;
create index on prtx2 (b);
create index on prtx2 (c);
analyze prtx1;
analyze prtx2;

explain (costs off)
select * from prtx1
where not exists (select 1 from prtx2
                  where prtx2.a=prtx1.a and prtx2.b=prtx1.b and prtx2.c=123)
  and a<20 and c=120;

select * from prtx1
where not exists (select 1 from prtx2
                  where prtx2.a=prtx1.a and prtx2.b=prtx1.b and prtx2.c=123)
  and a<20 and c=120;

explain (costs off)
select * from prtx1
where not exists (select 1 from prtx2
                  where prtx2.a=prtx1.a and (prtx2.b=prtx1.b+1 or prtx2.c=99))
  and a<20 and c=91;

select * from prtx1
where not exists (select 1 from prtx2
                  where prtx2.a=prtx1.a and (prtx2.b=prtx1.b+1 or prtx2.c=99))
  and a<20 and c=91;
