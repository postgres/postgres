set enable_seqscan=off;

CREATE TABLE test_numeric (
	i numeric
);

INSERT INTO test_numeric VALUES (-2),(-1),(0),(1),(2),(3);

CREATE INDEX idx_numeric ON test_numeric USING gin (i);

SELECT * FROM test_numeric WHERE i<'1'::numeric ORDER BY i;
SELECT * FROM test_numeric WHERE i<='1'::numeric ORDER BY i;
SELECT * FROM test_numeric WHERE i='1'::numeric ORDER BY i;
SELECT * FROM test_numeric WHERE i>='1'::numeric ORDER BY i;
SELECT * FROM test_numeric WHERE i>'1'::numeric ORDER BY i;
