--
-- DMLs on test table
--

SET pg_stat_statements.track_utility = FALSE;

CREATE TEMP TABLE pgss_dml_tab (a int, b char(20));

INSERT INTO pgss_dml_tab VALUES(generate_series(1, 10), 'aaa');
UPDATE pgss_dml_tab SET b = 'bbb' WHERE a > 7;
DELETE FROM pgss_dml_tab WHERE a > 9;

-- explicit transaction
BEGIN;
UPDATE pgss_dml_tab SET b = '111' WHERE a = 1 ;
COMMIT;

BEGIN \;
UPDATE pgss_dml_tab SET b = '222' WHERE a = 2 \;
COMMIT ;

UPDATE pgss_dml_tab SET b = '333' WHERE a = 3 \;
UPDATE pgss_dml_tab SET b = '444' WHERE a = 4 ;

BEGIN \;
UPDATE pgss_dml_tab SET b = '555' WHERE a = 5 \;
UPDATE pgss_dml_tab SET b = '666' WHERE a = 6 \;
COMMIT ;

-- many INSERT values
INSERT INTO pgss_dml_tab (a, b) VALUES (1, 'a'), (2, 'b'), (3, 'c');

-- SELECT with constants
SELECT * FROM pgss_dml_tab WHERE a > 5 ORDER BY a ;

SELECT *
  FROM pgss_dml_tab
  WHERE a > 9
  ORDER BY a ;

-- these two need to be done on a different table
-- SELECT without constants
SELECT * FROM pgss_dml_tab ORDER BY a;

-- SELECT with IN clause
SELECT * FROM pgss_dml_tab WHERE a IN (1, 2, 3, 4, 5);

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- MERGE
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a AND st.a >= 4)
 WHEN MATCHED THEN UPDATE SET b = st.b || st.a::text;
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a AND st.a >= 4)
 WHEN MATCHED THEN UPDATE SET b = pgss_dml_tab.b || st.a::text;
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a AND st.a >= 4)
 WHEN MATCHED AND length(st.b) > 1 THEN UPDATE SET b = pgss_dml_tab.b || st.a::text;
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a)
 WHEN NOT MATCHED THEN INSERT (a, b) VALUES (0, NULL);
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a)
 WHEN NOT MATCHED THEN INSERT VALUES (0, NULL);	-- same as above
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a)
 WHEN NOT MATCHED THEN INSERT (b, a) VALUES (NULL, 0);
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a)
 WHEN NOT MATCHED THEN INSERT (a) VALUES (0);
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a AND st.a >= 4)
 WHEN MATCHED THEN DELETE;
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a AND st.a >= 4)
 WHEN MATCHED THEN DO NOTHING;
MERGE INTO pgss_dml_tab USING pgss_dml_tab st ON (st.a = pgss_dml_tab.a AND st.a >= 4)
 WHEN NOT MATCHED THEN DO NOTHING;

DROP TABLE pgss_dml_tab;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- check that [temp] table relation extensions are tracked as writes
CREATE TABLE pgss_extend_tab (a int, b text);
CREATE TEMP TABLE pgss_extend_temp_tab (a int, b text);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
INSERT INTO pgss_extend_tab (a, b) SELECT generate_series(1, 1000), 'something';
INSERT INTO pgss_extend_temp_tab (a, b) SELECT generate_series(1, 1000), 'something';
WITH sizes AS (
  SELECT
    pg_relation_size('pgss_extend_tab') / current_setting('block_size')::int8 AS rel_size,
    pg_relation_size('pgss_extend_temp_tab') / current_setting('block_size')::int8 AS temp_rel_size
)
SELECT
    SUM(local_blks_written) >= (SELECT temp_rel_size FROM sizes) AS temp_written_ok,
    SUM(local_blks_dirtied) >= (SELECT temp_rel_size FROM sizes) AS temp_dirtied_ok,
    SUM(shared_blks_written) >= (SELECT rel_size FROM sizes) AS written_ok,
    SUM(shared_blks_dirtied) >= (SELECT rel_size FROM sizes) AS dirtied_ok
FROM pg_stat_statements;

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
