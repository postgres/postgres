--
-- select_implicit.sql
--
-- Test cases for queries with ordering terms missing from the target list.
-- This used to be called "junkfilter.sql".
-- The parser uses the term "resjunk" to handle these cases.
-- - thomas 1998-07-09

-- load test data
CREATE TABLE test_missing_target (a int, b int, c char(8));
INSERT INTO test_missing_target VALUES (0, 1, 'XXXX');
INSERT INTO test_missing_target VALUES (1, 2, 'AAAA');
INSERT INTO test_missing_target VALUES (2, 2, 'AAAA');
INSERT INTO test_missing_target VALUES (3, 3, 'BBBB');
INSERT INTO test_missing_target VALUES (4, 3, 'BBBB');
INSERT INTO test_missing_target VALUES (5, 3, 'BBBB');
INSERT INTO test_missing_target VALUES (6, 4, 'CCCC');
INSERT INTO test_missing_target VALUES (7, 4, 'CCCC');
INSERT INTO test_missing_target VALUES (8, 4, 'CCCC');
INSERT INTO test_missing_target VALUES (9, 4, 'CCCC');


--   w/ existing GROUP BY target
SELECT c, count(*) FROM test_missing_target GROUP BY test_missing_target.c;

--   w/o existing GROUP BY target using a relation name in GROUP BY clause
SELECT count(*) FROM test_missing_target GROUP BY test_missing_target.c;

--   w/o existing GROUP BY target and w/o existing a different ORDER BY target
--   failure expected
SELECT count(*) FROM test_missing_target GROUP BY a ORDER BY b;

--   w/o existing GROUP BY target and w/o existing same ORDER BY target
SELECT count(*) FROM test_missing_target GROUP BY b ORDER BY b;

--   w/ existing GROUP BY target using a relation name in target
SELECT test_missing_target.b, count(*)
  FROM test_missing_target GROUP BY b ORDER BY b;

--   w/o existing GROUP BY target
SELECT c FROM test_missing_target ORDER BY a;

--   w/o existing ORDER BY target
SELECT count(*) FROM test_missing_target GROUP BY b ORDER BY b desc;

--   group using reference number
SELECT count(*) FROM test_missing_target ORDER BY 1 desc;

--   order using reference number
SELECT c, count(*) FROM test_missing_target GROUP BY 1;

--   group using reference number out of range
--   failure expected
SELECT c, count(*) FROM test_missing_target GROUP BY 3;

--   group w/o existing GROUP BY and ORDER BY target under ambigious condition
--   failure expected
SELECT count(*) FROM test_missing_target x, test_missing_target y 
	WHERE x.a = y.a
	GROUP BY b ORDER BY b;

--   group w/ existing GROUP BY target under ambigious condition
SELECT x.b, count(*) FROM test_missing_target x, test_missing_target y 
	WHERE x.a = y.a
	GROUP BY x.b;

--   group w/o existing GROUP BY target under ambigious condition
SELECT count(*) FROM test_missing_target x, test_missing_target y 
	WHERE x.a = y.a
	GROUP BY x.b;

--   group w/o existing GROUP BY target under ambigious condition
--   into a table
SELECT count(*) INTO TABLE test_missing_target2 
FROM test_missing_target x, test_missing_target y 
	WHERE x.a = y.a
	GROUP BY x.b;
SELECT * FROM test_missing_target2;

--   Cleanup
DROP TABLE test_missing_target;
DROP TABLE test_missing_target2;

