set enable_seqscan=off;

CREATE TABLE test_float8 (
	i float8
);

INSERT INTO test_float8 VALUES (-2),(-1),(0),(1),(2),(3);

CREATE INDEX idx_float8 ON test_float8 USING gin (i);

SELECT * FROM test_float8 WHERE i<1::float8 ORDER BY i;
SELECT * FROM test_float8 WHERE i<=1::float8 ORDER BY i;
SELECT * FROM test_float8 WHERE i=1::float8 ORDER BY i;
SELECT * FROM test_float8 WHERE i>=1::float8 ORDER BY i;
SELECT * FROM test_float8 WHERE i>1::float8 ORDER BY i;
