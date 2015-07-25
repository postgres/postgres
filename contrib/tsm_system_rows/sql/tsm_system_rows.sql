CREATE EXTENSION tsm_system_rows;

CREATE TABLE test_tablesample (id int, name text);
INSERT INTO test_tablesample SELECT i, repeat(i::text, 1000)
  FROM generate_series(0, 30) s(i);
ANALYZE test_tablesample;

SELECT count(*) FROM test_tablesample TABLESAMPLE system_rows (0);
SELECT count(*) FROM test_tablesample TABLESAMPLE system_rows (1);
SELECT count(*) FROM test_tablesample TABLESAMPLE system_rows (10);
SELECT count(*) FROM test_tablesample TABLESAMPLE system_rows (100);

-- bad parameters should get through planning, but not execution:
EXPLAIN (COSTS OFF)
SELECT id FROM test_tablesample TABLESAMPLE system_rows (-1);

SELECT id FROM test_tablesample TABLESAMPLE system_rows (-1);

-- fail, this method is not repeatable:
SELECT * FROM test_tablesample TABLESAMPLE system_rows (10) REPEATABLE (0);

-- but a join should be allowed:
EXPLAIN (COSTS OFF)
SELECT * FROM
  (VALUES (0),(10),(100)) v(nrows),
  LATERAL (SELECT count(*) FROM test_tablesample
           TABLESAMPLE system_rows (nrows)) ss;

SELECT * FROM
  (VALUES (0),(10),(100)) v(nrows),
  LATERAL (SELECT count(*) FROM test_tablesample
           TABLESAMPLE system_rows (nrows)) ss;

CREATE VIEW vv AS
  SELECT count(*) FROM test_tablesample TABLESAMPLE system_rows (20);

SELECT * FROM vv;

DROP EXTENSION tsm_system_rows;  -- fail, view depends on extension
