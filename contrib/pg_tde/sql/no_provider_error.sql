CREATE EXTENSION pg_tde;

-- should fail
CREATE TABLE t1 (n INT) USING tde_heap;

-- should work
CREATE TABLE t2 (n INT) USING heap;

DROP TABLE t2;

DROP EXTENSION pg_tde;
