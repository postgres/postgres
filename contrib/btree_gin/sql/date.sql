set enable_seqscan=off;

CREATE TABLE test_date (
	i date
);

INSERT INTO test_date VALUES
	( '2004-10-23' ),
	( '2004-10-24' ),
	( '2004-10-25' ),
	( '2004-10-26' ),
	( '2004-10-27' ),
	( '2004-10-28' )
;

CREATE INDEX idx_date ON test_date USING gin (i);

SELECT * FROM test_date WHERE i<'2004-10-26'::date ORDER BY i;
SELECT * FROM test_date WHERE i<='2004-10-26'::date ORDER BY i;
SELECT * FROM test_date WHERE i='2004-10-26'::date ORDER BY i;
SELECT * FROM test_date WHERE i>='2004-10-26'::date ORDER BY i;
SELECT * FROM test_date WHERE i>'2004-10-26'::date ORDER BY i;

explain (costs off)
SELECT * FROM test_date WHERE i<'2004-10-26'::timestamp ORDER BY i;

SELECT * FROM test_date WHERE i<'2004-10-26'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i<='2004-10-26'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i='2004-10-26'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i>='2004-10-26'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i>'2004-10-26'::timestamp ORDER BY i;

explain (costs off)
SELECT * FROM test_date WHERE i<'2004-10-26'::timestamptz ORDER BY i;

SELECT * FROM test_date WHERE i<'2004-10-26'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i<='2004-10-26'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i='2004-10-26'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i>='2004-10-26'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i>'2004-10-26'::timestamptz ORDER BY i;

-- Check endpoint and out-of-range cases

INSERT INTO test_date VALUES ('-infinity'), ('infinity');
SELECT gin_clean_pending_list('idx_date');

SELECT * FROM test_date WHERE i<'-infinity'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i<='-infinity'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i='-infinity'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i>='-infinity'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i>'-infinity'::timestamp ORDER BY i;

SELECT * FROM test_date WHERE i<'infinity'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i<='infinity'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i='infinity'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i>='infinity'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i>'infinity'::timestamp ORDER BY i;

SELECT * FROM test_date WHERE i<'-infinity'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i<='-infinity'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i='-infinity'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i>='-infinity'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i>'-infinity'::timestamptz ORDER BY i;

SELECT * FROM test_date WHERE i<'infinity'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i<='infinity'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i='infinity'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i>='infinity'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i>'infinity'::timestamptz ORDER BY i;

-- Check rounding cases
-- '2004-10-25 00:00:01' rounds to '2004-10-25' for date.
-- '2004-10-25 23:59:59' also rounds to '2004-10-25',
-- so it's the same case as '2004-10-25 00:00:01'

SELECT * FROM test_date WHERE i < '2004-10-25 00:00:01'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i <= '2004-10-25 00:00:01'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i = '2004-10-25 00:00:01'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i > '2004-10-25 00:00:01'::timestamp ORDER BY i;
SELECT * FROM test_date WHERE i >= '2004-10-25 00:00:01'::timestamp ORDER BY i;

SELECT * FROM test_date WHERE i < '2004-10-25 00:00:01'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i <= '2004-10-25 00:00:01'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i = '2004-10-25 00:00:01'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i > '2004-10-25 00:00:01'::timestamptz ORDER BY i;
SELECT * FROM test_date WHERE i >= '2004-10-25 00:00:01'::timestamptz ORDER BY i;
