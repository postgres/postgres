set enable_seqscan=off;

CREATE TABLE test_time (
	i time
);

INSERT INTO test_time VALUES
	( '03:55:08' ),
	( '04:55:08' ),
	( '05:55:08' ),
	( '08:55:08' ),
	( '09:55:08' ),
	( '10:55:08' )
;

CREATE INDEX idx_time ON test_time USING gin (i);

SELECT * FROM test_time WHERE i<'08:55:08'::time ORDER BY i;
SELECT * FROM test_time WHERE i<='08:55:08'::time ORDER BY i;
SELECT * FROM test_time WHERE i='08:55:08'::time ORDER BY i;
SELECT * FROM test_time WHERE i>='08:55:08'::time ORDER BY i;
SELECT * FROM test_time WHERE i>'08:55:08'::time ORDER BY i;
