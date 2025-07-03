set enable_seqscan=off;

CREATE TABLE test_timestamptz (
	i timestamptz
);

INSERT INTO test_timestamptz VALUES
	( '2004-10-26 03:55:08' ),
	( '2004-10-26 04:55:08' ),
	( '2004-10-26 05:55:08' ),
	( '2004-10-26 08:55:08' ),
	( '2004-10-27 09:55:08' ),
	( '2004-10-27 10:55:08' )
;

CREATE INDEX idx_timestamptz ON test_timestamptz USING gin (i);

SELECT * FROM test_timestamptz WHERE i<'2004-10-26 08:55:08'::timestamptz ORDER BY i;
SELECT * FROM test_timestamptz WHERE i<='2004-10-26 08:55:08'::timestamptz ORDER BY i;
SELECT * FROM test_timestamptz WHERE i='2004-10-26 08:55:08'::timestamptz ORDER BY i;
SELECT * FROM test_timestamptz WHERE i>='2004-10-26 08:55:08'::timestamptz ORDER BY i;
SELECT * FROM test_timestamptz WHERE i>'2004-10-26 08:55:08'::timestamptz ORDER BY i;

explain (costs off)
SELECT * FROM test_timestamptz WHERE i<'2004-10-27'::date ORDER BY i;

SELECT * FROM test_timestamptz WHERE i<'2004-10-27'::date ORDER BY i;
SELECT * FROM test_timestamptz WHERE i<='2004-10-27'::date ORDER BY i;
SELECT * FROM test_timestamptz WHERE i='2004-10-27'::date ORDER BY i;
SELECT * FROM test_timestamptz WHERE i>='2004-10-27'::date ORDER BY i;
SELECT * FROM test_timestamptz WHERE i>'2004-10-27'::date ORDER BY i;

explain (costs off)
SELECT * FROM test_timestamptz WHERE i<'2004-10-26 08:55:08'::timestamp ORDER BY i;

SELECT * FROM test_timestamptz WHERE i<'2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamptz WHERE i<='2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamptz WHERE i='2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamptz WHERE i>='2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamptz WHERE i>'2004-10-26 08:55:08'::timestamp ORDER BY i;
