CREATE EXTENSION tsm_system_time;

CREATE TABLE test_tablesample (id int, name text);
INSERT INTO test_tablesample SELECT i, repeat(i::text, 1000)
  FROM generate_series(0, 30) s(i);
ANALYZE test_tablesample;

-- It's a bit tricky to test SYSTEM_TIME in a platform-independent way.
-- We can test the zero-time corner case ...
SELECT count(*) FROM test_tablesample TABLESAMPLE system_time (0);
-- ... and we assume that this will finish before running out of time:
SELECT count(*) FROM test_tablesample TABLESAMPLE system_time (100000);

-- bad parameters should get through planning, but not execution:
EXPLAIN (COSTS OFF)
SELECT id FROM test_tablesample TABLESAMPLE system_time (-1);

SELECT id FROM test_tablesample TABLESAMPLE system_time (-1);

-- fail, this method is not repeatable:
SELECT * FROM test_tablesample TABLESAMPLE system_time (10) REPEATABLE (0);

-- since it's not repeatable, we expect a Materialize node in these plans:
EXPLAIN (COSTS OFF)
SELECT * FROM
  (VALUES (0),(100000)) v(time),
  LATERAL (SELECT COUNT(*) FROM test_tablesample
           TABLESAMPLE system_time (100000)) ss;

SELECT * FROM
  (VALUES (0),(100000)) v(time),
  LATERAL (SELECT COUNT(*) FROM test_tablesample
           TABLESAMPLE system_time (100000)) ss;

EXPLAIN (COSTS OFF)
SELECT * FROM
  (VALUES (0),(100000)) v(time),
  LATERAL (SELECT COUNT(*) FROM test_tablesample
           TABLESAMPLE system_time (time)) ss;

SELECT * FROM
  (VALUES (0),(100000)) v(time),
  LATERAL (SELECT COUNT(*) FROM test_tablesample
           TABLESAMPLE system_time (time)) ss;

CREATE VIEW vv AS
  SELECT * FROM test_tablesample TABLESAMPLE system_time (20);

EXPLAIN (COSTS OFF) SELECT * FROM vv;

DROP EXTENSION tsm_system_time;  -- fail, view depends on extension
