set enable_seqscan=off;

CREATE TABLE test_timetz (
	i timetz
);

INSERT INTO test_timetz VALUES
	( '03:55:08 GMT+2' ),
	( '04:55:08 GMT+2' ),
	( '05:55:08 GMT+2' ),
	( '08:55:08 GMT+2' ),
	( '09:55:08 GMT+2' ),
	( '10:55:08 GMT+2' )
;

CREATE INDEX idx_timetz ON test_timetz USING gin (i);

SELECT * FROM test_timetz WHERE i<'08:55:08 GMT+2'::timetz ORDER BY i;
SELECT * FROM test_timetz WHERE i<='08:55:08 GMT+2'::timetz ORDER BY i;
SELECT * FROM test_timetz WHERE i='08:55:08 GMT+2'::timetz ORDER BY i;
SELECT * FROM test_timetz WHERE i>='08:55:08 GMT+2'::timetz ORDER BY i;
SELECT * FROM test_timetz WHERE i>'08:55:08 GMT+2'::timetz ORDER BY i;
