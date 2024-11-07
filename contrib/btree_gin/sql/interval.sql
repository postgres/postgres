set enable_seqscan=off;

CREATE TABLE test_interval (
	i interval
);

INSERT INTO test_interval VALUES
	( '-178000000 years' ),
	( '03:55:08' ),
	( '04:55:08' ),
	( '05:55:08' ),
	( '08:55:08' ),
	( '09:55:08' ),
	( '10:55:08' ),
	( '178000000 years' )
;

CREATE INDEX idx_interval ON test_interval USING gin (i);

SELECT * FROM test_interval WHERE i<'08:55:08'::interval ORDER BY i;
SELECT * FROM test_interval WHERE i<='08:55:08'::interval ORDER BY i;
SELECT * FROM test_interval WHERE i='08:55:08'::interval ORDER BY i;
SELECT * FROM test_interval WHERE i>='08:55:08'::interval ORDER BY i;
SELECT * FROM test_interval WHERE i>'08:55:08'::interval ORDER BY i;
