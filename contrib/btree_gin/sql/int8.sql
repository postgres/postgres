set enable_seqscan=off;

CREATE TABLE test_int8 (
	i int8
);

INSERT INTO test_int8 VALUES (-2),(-1),(0),(1),(2),(3);

CREATE INDEX idx_int8 ON test_int8 USING gin (i);

SELECT * FROM test_int8 WHERE i<1::int8 ORDER BY i;
SELECT * FROM test_int8 WHERE i<=1::int8 ORDER BY i;
SELECT * FROM test_int8 WHERE i=1::int8 ORDER BY i;
SELECT * FROM test_int8 WHERE i>=1::int8 ORDER BY i;
SELECT * FROM test_int8 WHERE i>1::int8 ORDER BY i;

explain (costs off)
SELECT * FROM test_int8 WHERE i<1::int2 ORDER BY i;

SELECT * FROM test_int8 WHERE i<1::int2 ORDER BY i;
SELECT * FROM test_int8 WHERE i<=1::int2 ORDER BY i;
SELECT * FROM test_int8 WHERE i=1::int2 ORDER BY i;
SELECT * FROM test_int8 WHERE i>=1::int2 ORDER BY i;
SELECT * FROM test_int8 WHERE i>1::int2 ORDER BY i;

explain (costs off)
SELECT * FROM test_int8 WHERE i<1::int4 ORDER BY i;

SELECT * FROM test_int8 WHERE i<1::int4 ORDER BY i;
SELECT * FROM test_int8 WHERE i<=1::int4 ORDER BY i;
SELECT * FROM test_int8 WHERE i=1::int4 ORDER BY i;
SELECT * FROM test_int8 WHERE i>=1::int4 ORDER BY i;
SELECT * FROM test_int8 WHERE i>1::int4 ORDER BY i;
