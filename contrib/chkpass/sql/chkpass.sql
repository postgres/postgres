CREATE EXTENSION chkpass;

CREATE TABLE test (i int, p chkpass);
INSERT INTO test VALUES (1, 'hello'), (2, 'goodbye');

SELECT i, p = 'hello' AS "hello?" FROM test;
SELECT i, p <> 'hello' AS "!hello?" FROM test;
