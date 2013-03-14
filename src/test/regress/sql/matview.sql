-- create a table to use as a basis for views and materialized views in various combinations
CREATE TABLE t (id int NOT NULL PRIMARY KEY, type text NOT NULL, amt numeric NOT NULL);
INSERT INTO t VALUES
  (1, 'x', 2),
  (2, 'x', 3),
  (3, 'y', 5),
  (4, 'y', 7),
  (5, 'z', 11);

-- we want a view based on the table, too, since views present additional challenges
CREATE VIEW tv AS SELECT type, sum(amt) AS totamt FROM t GROUP BY type;
SELECT * FROM tv;

-- create a materialized view with no data, and confirm correct behavior
EXPLAIN (costs off)
  CREATE MATERIALIZED VIEW tm AS SELECT type, sum(amt) AS totamt FROM t GROUP BY type WITH NO DATA;
CREATE MATERIALIZED VIEW tm AS SELECT type, sum(amt) AS totamt FROM t GROUP BY type WITH NO DATA;
SELECT pg_relation_is_scannable('tm'::regclass);
SELECT * FROM tm;
REFRESH MATERIALIZED VIEW tm;
SELECT pg_relation_is_scannable('tm'::regclass);
CREATE UNIQUE INDEX tm_type ON tm (type);
SELECT * FROM tm;

-- create various views
EXPLAIN (costs off)
  CREATE MATERIALIZED VIEW tvm AS SELECT * FROM tv;
CREATE MATERIALIZED VIEW tvm AS SELECT * FROM tv;
SELECT * FROM tvm;
CREATE MATERIALIZED VIEW tmm AS SELECT sum(totamt) AS grandtot FROM tm;
CREATE MATERIALIZED VIEW tvmm AS SELECT sum(totamt) AS grandtot FROM tvm;
CREATE VIEW tvv AS SELECT sum(totamt) AS grandtot FROM tv;
EXPLAIN (costs off)
  CREATE MATERIALIZED VIEW tvvm AS SELECT * FROM tvv;
CREATE MATERIALIZED VIEW tvvm AS SELECT * FROM tvv;
CREATE VIEW tvvmv AS SELECT * FROM tvvm;
CREATE MATERIALIZED VIEW bb AS SELECT * FROM tvvmv;
CREATE INDEX aa ON bb (grandtot);

-- check that plans seem reasonable
\d+ tvm
\d+ tvm
\d+ tvvm
\d+ bb

-- test schema behavior
CREATE SCHEMA mvschema;
ALTER MATERIALIZED VIEW tvm SET SCHEMA mvschema;
\d+ tvm
\d+ tvmm
SET search_path = mvschema, public;
\d+ tvm

-- modify the underlying table data
INSERT INTO t VALUES (6, 'z', 13);

-- confirm pre- and post-refresh contents of fairly simple materialized views
SELECT * FROM tm ORDER BY type;
SELECT * FROM tvm ORDER BY type;
REFRESH MATERIALIZED VIEW tm;
REFRESH MATERIALIZED VIEW tvm;
SELECT * FROM tm ORDER BY type;
SELECT * FROM tvm ORDER BY type;
RESET search_path;

-- confirm pre- and post-refresh contents of nested materialized views
EXPLAIN (costs off)
  SELECT * FROM tmm;
EXPLAIN (costs off)
  SELECT * FROM tvmm;
EXPLAIN (costs off)
  SELECT * FROM tvvm;
SELECT * FROM tmm;
SELECT * FROM tvmm;
SELECT * FROM tvvm;
REFRESH MATERIALIZED VIEW tmm;
REFRESH MATERIALIZED VIEW tvmm;
REFRESH MATERIALIZED VIEW tvvm;
EXPLAIN (costs off)
  SELECT * FROM tmm;
EXPLAIN (costs off)
  SELECT * FROM tvmm;
EXPLAIN (costs off)
  SELECT * FROM tvvm;
SELECT * FROM tmm;
SELECT * FROM tvmm;
SELECT * FROM tvvm;

-- test diemv when the mv does not exist
DROP MATERIALIZED VIEW IF EXISTS tum;

-- make sure that an unlogged materialized view works (in the absence of a crash)
CREATE UNLOGGED MATERIALIZED VIEW tum AS SELECT type, sum(amt) AS totamt FROM t GROUP BY type WITH NO DATA;
SELECT pg_relation_is_scannable('tum'::regclass);
SELECT * FROM tum;
REFRESH MATERIALIZED VIEW tum;
SELECT pg_relation_is_scannable('tum'::regclass);
SELECT * FROM tum;
REFRESH MATERIALIZED VIEW tum WITH NO DATA;
SELECT pg_relation_is_scannable('tum'::regclass);
SELECT * FROM tum;
REFRESH MATERIALIZED VIEW tum WITH DATA;
SELECT pg_relation_is_scannable('tum'::regclass);
SELECT * FROM tum;

-- test join of mv and view
SELECT type, m.totamt AS mtot, v.totamt AS vtot FROM tm m LEFT JOIN tv v USING (type);

-- test diemv when the mv does exist
DROP MATERIALIZED VIEW IF EXISTS tum;

-- make sure that dependencies are reported properly when they block the drop
DROP TABLE t;

-- make sure dependencies are dropped and reported
-- and make sure that transactional behavior is correct on rollback
-- incidentally leaving some interesting materialized views for pg_dump testing
BEGIN;
DROP TABLE t CASCADE;
ROLLBACK;

-- some additional tests not using base tables
CREATE VIEW v_test1 AS SELECT 1 moo;
CREATE VIEW v_test2 AS SELECT moo, 2*moo FROM v_test1 UNION ALL SELECT moo, 3*moo FROM v_test1;
\d+ v_test2
CREATE MATERIALIZED VIEW mv_test2 AS SELECT moo, 2*moo FROM v_test2 UNION ALL SELECT moo, 3*moo FROM v_test2;
\d+ mv_test2
CREATE MATERIALIZED VIEW mv_test3 AS SELECT * FROM mv_test2 WHERE moo = 12345;
SELECT pg_relation_is_scannable('mv_test3'::regclass);

DROP VIEW v_test1 CASCADE;
