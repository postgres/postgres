set enable_seqscan=off;

CREATE TABLE test_float4 (
	i float4
);

INSERT INTO test_float4 VALUES (-2),(-1),(0),(1),(2),(3);

CREATE INDEX idx_float4 ON test_float4 USING gin (i);

SELECT * FROM test_float4 WHERE i<1::float4 ORDER BY i;
SELECT * FROM test_float4 WHERE i<=1::float4 ORDER BY i;
SELECT * FROM test_float4 WHERE i=1::float4 ORDER BY i;
SELECT * FROM test_float4 WHERE i>=1::float4 ORDER BY i;
SELECT * FROM test_float4 WHERE i>1::float4 ORDER BY i;

explain (costs off)
SELECT * FROM test_float4 WHERE i<1::float8 ORDER BY i;

SELECT * FROM test_float4 WHERE i<1::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i<=1::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i=1::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>=1::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>1::float8 ORDER BY i;

-- Check endpoint and out-of-range cases

INSERT INTO test_float4 VALUES ('NaN'), ('Inf'), ('-Inf');
SELECT gin_clean_pending_list('idx_float4');

SELECT * FROM test_float4 WHERE i<'-Inf'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i<='-Inf'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i='-Inf'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>='-Inf'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>'-Inf'::float8 ORDER BY i;

SELECT * FROM test_float4 WHERE i<'Inf'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i<='Inf'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i='Inf'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>='Inf'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>'Inf'::float8 ORDER BY i;

SELECT * FROM test_float4 WHERE i<'1e300'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i<='1e300'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i='1e300'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>='1e300'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>'1e300'::float8 ORDER BY i;

SELECT * FROM test_float4 WHERE i<'NaN'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i<='NaN'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i='NaN'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>='NaN'::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i>'NaN'::float8 ORDER BY i;

-- Check rounding cases
-- 1e-300 rounds to 0 for float4 but not for float8

SELECT * FROM test_float4 WHERE i < -1e-300::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i <= -1e-300::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i = -1e-300::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i > -1e-300::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i >= -1e-300::float8 ORDER BY i;

SELECT * FROM test_float4 WHERE i < 1e-300::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i <= 1e-300::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i = 1e-300::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i > 1e-300::float8 ORDER BY i;
SELECT * FROM test_float4 WHERE i >= 1e-300::float8 ORDER BY i;
