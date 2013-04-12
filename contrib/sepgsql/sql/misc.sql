--
-- Regression Test for Misc Permission Checks
--

LOAD '$libdir/sepgsql';		-- failed

--
-- Permissions to execute functions
--
CREATE TABLE t1 (x int, y text);
INSERT INTO t1 (SELECT x, md5(x::text) FROM generate_series(1,100) x);

SET sepgsql.debug_audit = on;
SET client_min_messages = log;

-- regular function and operators
SELECT * FROM t1 WHERE x > 50 AND y like '%64%';

-- aggregate function
SELECT MIN(x), AVG(x) FROM t1;

-- window function
SELECT row_number() OVER (order by x), * FROM t1 WHERE y like '%86%';

RESET sepgsql.debug_audit;
RESET client_min_messages;
--
-- Cleanup
--
DROP TABLE IF EXISTS t1 CASCADE;
