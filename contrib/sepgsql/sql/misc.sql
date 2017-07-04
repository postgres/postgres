--
-- Regression Test for Misc Permission Checks
--

LOAD '$libdir/sepgsql';		-- failed

--
-- Permissions to execute functions
--
CREATE TABLE t1 (x int, y text);
INSERT INTO t1 (SELECT x, md5(x::text) FROM generate_series(1,100) x);

CREATE TABLE t1p (o int, p text) PARTITION BY RANGE (o);
CREATE TABLE t1p_ones PARTITION OF t1p FOR VALUES FROM ('0') TO ('10');
CREATE TABLE t1p_tens PARTITION OF t1p FOR VALUES FROM ('10') TO ('100');
INSERT INTO t1p (SELECT x, md5(x::text) FROM generate_series(0,99) x);

SET sepgsql.debug_audit = on;
SET client_min_messages = log;

-- regular function and operators
SELECT * FROM t1 WHERE x > 50 AND y like '%64%';
SELECT * FROM t1p WHERE o > 50 AND p like '%64%';
SELECT * FROM t1p_ones WHERE o > 50 AND p like '%64%';
SELECT * FROM t1p_tens WHERE o > 50 AND p like '%64%';

-- aggregate function
SELECT MIN(x), AVG(x) FROM t1;
SELECT MIN(o), AVG(o) FROM t1p;
SELECT MIN(o), AVG(o) FROM t1p_ones;
SELECT MIN(o), AVG(o) FROM t1p_tens;

-- window function
SELECT row_number() OVER (order by x), * FROM t1 WHERE y like '%86%';
SELECT row_number() OVER (order by o), * FROM t1p WHERE p like '%86%';
SELECT row_number() OVER (order by o), * FROM t1p_ones WHERE p like '%86%';
SELECT row_number() OVER (order by o), * FROM t1p_tens WHERE p like '%86%';

RESET sepgsql.debug_audit;
RESET client_min_messages;
--
-- Cleanup
--
DROP TABLE IF EXISTS t1 CASCADE;
DROP TABLE IF EXISTS t1p CASCADE;
