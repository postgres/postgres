-- numeric check

CREATE TABLE numerictmp (a numeric);

\copy numerictmp from 'data/int8.data'
\copy numerictmp from 'data/numeric.data'
\copy numerictmp from 'data/float8.data'

SET enable_seqscan=on;

SELECT count(*) FROM numerictmp WHERE a <  -1890.0;

SELECT count(*) FROM numerictmp WHERE a <= -1890.0;

SELECT count(*) FROM numerictmp WHERE a  = -1890.0;

SELECT count(*) FROM numerictmp WHERE a >= -1890.0;

SELECT count(*) FROM numerictmp WHERE a >  -1890.0;


SELECT count(*) FROM numerictmp WHERE a <  'NaN' ;

SELECT count(*) FROM numerictmp WHERE a <= 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a  = 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a >= 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a >  'NaN' ;

SELECT count(*) FROM numerictmp WHERE a <  0 ;

SELECT count(*) FROM numerictmp WHERE a <= 0 ;

SELECT count(*) FROM numerictmp WHERE a  = 0 ;

SELECT count(*) FROM numerictmp WHERE a >= 0 ;

SELECT count(*) FROM numerictmp WHERE a >  0 ;


CREATE INDEX numericidx ON numerictmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM numerictmp WHERE a <  -1890.0;

SELECT count(*) FROM numerictmp WHERE a <= -1890.0;

SELECT count(*) FROM numerictmp WHERE a  = -1890.0;

SELECT count(*) FROM numerictmp WHERE a >= -1890.0;

SELECT count(*) FROM numerictmp WHERE a >  -1890.0;


SELECT count(*) FROM numerictmp WHERE a <  'NaN' ;

SELECT count(*) FROM numerictmp WHERE a <= 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a  = 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a >= 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a >  'NaN' ;


SELECT count(*) FROM numerictmp WHERE a <  0 ;

SELECT count(*) FROM numerictmp WHERE a <= 0 ;

SELECT count(*) FROM numerictmp WHERE a  = 0 ;

SELECT count(*) FROM numerictmp WHERE a >= 0 ;

SELECT count(*) FROM numerictmp WHERE a >  0 ;

-- Test index-only scans
SET enable_bitmapscan=off;
EXPLAIN (COSTS OFF)
SELECT * FROM numerictmp WHERE a BETWEEN 1 AND 300 ORDER BY a;
SELECT * FROM numerictmp WHERE a BETWEEN 1 AND 300 ORDER BY a;

-- A key width that overflows the numeric format, or is infinite or NaN
-- because a bound is, must not cause a failure.
CREATE TABLE numericwide (a numeric);
INSERT INTO numericwide SELECT g FROM generate_series(1, 1000) g;
CREATE INDEX numericwideidx ON numericwide USING gist (a);
INSERT INTO numericwide VALUES (('9' || repeat('9', 131071))::numeric),
                               (('-9' || repeat('9', 131071))::numeric),
                               ('infinity'), ('-infinity'),
                               ('NaN');
SET enable_seqscan = off;
SELECT count(*) FROM numericwide WHERE a > 0;
RESET enable_seqscan;
