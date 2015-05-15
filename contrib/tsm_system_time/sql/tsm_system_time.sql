CREATE EXTENSION tsm_system_time;

CREATE TABLE test_tablesample (id int, name text) WITH (fillfactor=10); -- force smaller pages so we don't have to load too much data to get multiple pages

INSERT INTO test_tablesample SELECT i, repeat(i::text, 1000) FROM generate_series(0, 30) s(i) ORDER BY i;
ANALYZE test_tablesample;

SELECT count(*) FROM test_tablesample TABLESAMPLE system_time (1000);
SELECT id FROM test_tablesample TABLESAMPLE system_time (1000) REPEATABLE (5432);

EXPLAIN SELECT id FROM test_tablesample TABLESAMPLE system_time (100) REPEATABLE (10);

-- done
DROP TABLE test_tablesample CASCADE;
