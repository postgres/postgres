set enable_seqscan=off;

CREATE TABLE test_timestamp (
	i timestamp
);

INSERT INTO test_timestamp VALUES
	( '2004-10-26 03:55:08' ),
	( '2004-10-26 04:55:08' ),
	( '2004-10-26 05:55:08' ),
	( '2004-10-26 08:55:08' ),
	( '2004-10-27 09:55:08' ),
	( '2004-10-27 10:55:08' )
;

CREATE INDEX idx_timestamp ON test_timestamp USING gin (i);

SELECT * FROM test_timestamp WHERE i<'2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamp WHERE i<='2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamp WHERE i='2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamp WHERE i>='2004-10-26 08:55:08'::timestamp ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'2004-10-26 08:55:08'::timestamp ORDER BY i;

explain (costs off)
SELECT * FROM test_timestamp WHERE i<'2004-10-27'::date ORDER BY i;

SELECT * FROM test_timestamp WHERE i<'2004-10-27'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i<='2004-10-27'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i='2004-10-27'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i>='2004-10-27'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'2004-10-27'::date ORDER BY i;

explain (costs off)
SELECT * FROM test_timestamp WHERE i<'2004-10-26 08:55:08'::timestamptz ORDER BY i;

SELECT * FROM test_timestamp WHERE i<'2004-10-26 08:55:08'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i<='2004-10-26 08:55:08'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i='2004-10-26 08:55:08'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i>='2004-10-26 08:55:08'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'2004-10-26 08:55:08'::timestamptz ORDER BY i;

-- Check endpoint and out-of-range cases

INSERT INTO test_timestamp VALUES ('-infinity'), ('infinity');
SELECT gin_clean_pending_list('idx_timestamp');

SELECT * FROM test_timestamp WHERE i<'-infinity'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i<='-infinity'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i='-infinity'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i>='-infinity'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'-infinity'::date ORDER BY i;

SELECT * FROM test_timestamp WHERE i<'infinity'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i<='infinity'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i='infinity'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i>='infinity'::date ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'infinity'::date ORDER BY i;

SELECT * FROM test_timestamp WHERE i<'-infinity'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i<='-infinity'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i='-infinity'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i>='-infinity'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'-infinity'::timestamptz ORDER BY i;

SELECT * FROM test_timestamp WHERE i<'infinity'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i<='infinity'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i='infinity'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i>='infinity'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'infinity'::timestamptz ORDER BY i;

-- This PST timestamptz will underflow if converted to timestamp
SELECT * FROM test_timestamp WHERE i<='4714-11-23 17:00 BC'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'4714-11-23 17:00 BC'::timestamptz ORDER BY i;

-- Check that we aren't fooled by ambiguous timestamps near a DST transition
INSERT INTO test_timestamp VALUES
  ('2026-11-01 00:50'), ('2026-11-01 01:10'), ('2026-11-01 01:20'),
  ('2026-11-01 01:40'), ('2026-11-01 02:10');
SET TimeZone = 'America/New_York';
SELECT * FROM test_timestamp WHERE i>='2026-11-01 01:30-04'::timestamptz ORDER BY i;
SELECT * FROM test_timestamp WHERE i>'2026-11-01 01:30-04'::timestamptz ORDER BY i;
RESET TimeZone;
