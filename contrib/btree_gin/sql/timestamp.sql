set enable_seqscan=off;

CREATE TABLE test_timestamp (
	i timestamp
);

INSERT INTO test_timestamp VALUES 
	( '2004-10-26 03:55:08' ),
	( '2004-10-26 04:55:08' ),
	( '2004-10-26 05:55:08' ),
	( '2004-10-26 08:55:08' ),
	( '2004-10-26 09:55:08' ),
	( '2004-10-26 10:55:08' )
;

CREATE INDEX idx_timestamp ON test_timestamp USING gin (i);

SELECT * FROM test_timestamp WHERE i<'2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamp WHERE i<='2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamp WHERE i='2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamp WHERE i>='2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'2004-10-26 08:55:08'::timestamp ORDER BY i;
