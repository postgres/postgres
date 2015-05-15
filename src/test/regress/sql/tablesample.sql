CREATE TABLE test_tablesample (id int, name text) WITH (fillfactor=10); -- force smaller pages so we don't have to load too much data to get multiple pages

INSERT INTO test_tablesample SELECT i, repeat(i::text, 200) FROM generate_series(0, 9) s(i) ORDER BY i;

SELECT t.id FROM test_tablesample AS t TABLESAMPLE SYSTEM (50) REPEATABLE (10);
SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (100.0/11) REPEATABLE (9999);
SELECT count(*) FROM test_tablesample TABLESAMPLE SYSTEM (100);
SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (50) REPEATABLE (100);
SELECT id FROM test_tablesample TABLESAMPLE BERNOULLI (50) REPEATABLE (100);
SELECT id FROM test_tablesample TABLESAMPLE BERNOULLI (5.5) REPEATABLE (1);

CREATE VIEW test_tablesample_v1 AS SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (10*2) REPEATABLE (2);
CREATE VIEW test_tablesample_v2 AS SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (99);
SELECT pg_get_viewdef('test_tablesample_v1'::regclass);
SELECT pg_get_viewdef('test_tablesample_v2'::regclass);

BEGIN;
DECLARE tablesample_cur CURSOR FOR SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (50) REPEATABLE (100);
FETCH FIRST FROM tablesample_cur;
FETCH NEXT FROM tablesample_cur;
FETCH NEXT FROM tablesample_cur;

SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (50) REPEATABLE (10);

FETCH NEXT FROM tablesample_cur;
FETCH NEXT FROM tablesample_cur;
FETCH NEXT FROM tablesample_cur;

FETCH FIRST FROM tablesample_cur;
FETCH NEXT FROM tablesample_cur;
FETCH NEXT FROM tablesample_cur;
FETCH NEXT FROM tablesample_cur;
FETCH NEXT FROM tablesample_cur;
FETCH NEXT FROM tablesample_cur;

CLOSE tablesample_cur;
END;

EXPLAIN SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (50) REPEATABLE (10);
EXPLAIN SELECT * FROM test_tablesample_v1;

-- errors
SELECT id FROM test_tablesample TABLESAMPLE FOOBAR (1);

SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (50) REPEATABLE (NULL);

SELECT id FROM test_tablesample TABLESAMPLE BERNOULLI (-1);
SELECT id FROM test_tablesample TABLESAMPLE BERNOULLI (200);
SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (-1);
SELECT id FROM test_tablesample TABLESAMPLE SYSTEM (200);

SELECT id FROM test_tablesample_v1 TABLESAMPLE BERNOULLI (1);
INSERT INTO test_tablesample_v1 VALUES(1);

WITH query_select AS (SELECT * FROM test_tablesample)
SELECT * FROM query_select TABLESAMPLE BERNOULLI (5.5) REPEATABLE (1);

SELECT q.* FROM (SELECT * FROM test_tablesample) as q TABLESAMPLE BERNOULLI (5);

-- catalog sanity

SELECT *
FROM pg_tablesample_method
WHERE tsminit IS NULL
   OR tsmseqscan IS NULL
   OR tsmpagemode IS NULL
   OR tsmnextblock IS NULL
   OR tsmnexttuple IS NULL
   OR tsmend IS NULL
   OR tsmreset IS NULL
   OR tsmcost IS NULL;

-- done
DROP TABLE test_tablesample CASCADE;
