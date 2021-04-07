-- time check

CREATE TABLE timetmp (a time);

\copy timetmp from 'data/time.data'

SET enable_seqscan=on;

SELECT count(*) FROM timetmp WHERE a <  '10:57:11';

SELECT count(*) FROM timetmp WHERE a <= '10:57:11';

SELECT count(*) FROM timetmp WHERE a  = '10:57:11';

SELECT count(*) FROM timetmp WHERE a >= '10:57:11';

SELECT count(*) FROM timetmp WHERE a >  '10:57:11';

SELECT a, a <-> '10:57:11' FROM timetmp ORDER BY a <-> '10:57:11' LIMIT 3;

SET client_min_messages = DEBUG1;
CREATE INDEX timeidx ON timetmp USING gist ( a );
CREATE INDEX timeidx_b ON timetmp USING gist ( a ) WITH (buffering=on);
DROP INDEX timeidx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM timetmp WHERE a <  '10:57:11'::time;

SELECT count(*) FROM timetmp WHERE a <= '10:57:11'::time;

SELECT count(*) FROM timetmp WHERE a  = '10:57:11'::time;

SELECT count(*) FROM timetmp WHERE a >= '10:57:11'::time;

SELECT count(*) FROM timetmp WHERE a >  '10:57:11'::time;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '10:57:11' FROM timetmp ORDER BY a <-> '10:57:11' LIMIT 3;
SELECT a, a <-> '10:57:11' FROM timetmp ORDER BY a <-> '10:57:11' LIMIT 3;
