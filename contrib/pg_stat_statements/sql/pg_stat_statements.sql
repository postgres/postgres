CREATE EXTENSION pg_stat_statements;

CREATE TABLE test (a int, b char(20));

-- test the basic functionality of pg_stat_statements
SELECT pg_stat_statements_reset();

INSERT INTO test VALUES(generate_series(1, 10), 'aaa');
UPDATE test SET b = 'bbb' WHERE a > 5;

SELECT query, calls, rows from pg_stat_statements ORDER BY rows;

DROP TABLE test;

DROP EXTENSION pg_stat_statements;
