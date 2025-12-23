set enable_seqscan=off;

CREATE TABLE test_int2 (
	i int2
);

INSERT INTO test_int2 VALUES (-2),(-1),(0),(1),(2),(3);

CREATE INDEX idx_int2 ON test_int2 USING gin (i);

SELECT * FROM test_int2 WHERE i<1::int2 ORDER BY i;
SELECT * FROM test_int2 WHERE i<=1::int2 ORDER BY i;
SELECT * FROM test_int2 WHERE i=1::int2 ORDER BY i;
SELECT * FROM test_int2 WHERE i>=1::int2 ORDER BY i;
SELECT * FROM test_int2 WHERE i>1::int2 ORDER BY i;

explain (costs off)
SELECT * FROM test_int2 WHERE i<1::int4 ORDER BY i;

SELECT * FROM test_int2 WHERE i<1::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i<=1::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i=1::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i>=1::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i>1::int4 ORDER BY i;

explain (costs off)
SELECT * FROM test_int2 WHERE i<1::int8 ORDER BY i;

SELECT * FROM test_int2 WHERE i<1::int8 ORDER BY i;
SELECT * FROM test_int2 WHERE i<=1::int8 ORDER BY i;
SELECT * FROM test_int2 WHERE i=1::int8 ORDER BY i;
SELECT * FROM test_int2 WHERE i>=1::int8 ORDER BY i;
SELECT * FROM test_int2 WHERE i>1::int8 ORDER BY i;

-- Check endpoint and out-of-range cases

INSERT INTO test_int2 VALUES ((-32768)::int2),(32767);
SELECT gin_clean_pending_list('idx_int2');

SELECT * FROM test_int2 WHERE i<(-32769)::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i<=(-32769)::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i=(-32769)::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i>=(-32769)::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i>(-32769)::int4 ORDER BY i;

SELECT * FROM test_int2 WHERE i<32768::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i<=32768::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i=32768::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i>=32768::int4 ORDER BY i;
SELECT * FROM test_int2 WHERE i>32768::int4 ORDER BY i;
