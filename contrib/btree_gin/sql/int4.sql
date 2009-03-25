set enable_seqscan=off;

CREATE TABLE test_int4 (
	i int4
);

INSERT INTO test_int4 VALUES (-2),(-1),(0),(1),(2),(3);

CREATE INDEX idx_int4 ON test_int4 USING gin (i);

SELECT * FROM test_int4 WHERE i<1::int4 ORDER BY i;
SELECT * FROM test_int4 WHERE i<=1::int4 ORDER BY i;
SELECT * FROM test_int4 WHERE i=1::int4 ORDER BY i;
SELECT * FROM test_int4 WHERE i>=1::int4 ORDER BY i;
SELECT * FROM test_int4 WHERE i>1::int4 ORDER BY i;
