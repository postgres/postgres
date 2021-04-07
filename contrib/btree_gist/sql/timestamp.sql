-- timestamp check

CREATE TABLE timestamptmp (a timestamp);

\copy timestamptmp from 'data/timestamp.data'

SET enable_seqscan=on;

SELECT count(*) FROM timestamptmp WHERE a <  '2004-10-26 08:55:08';

SELECT count(*) FROM timestamptmp WHERE a <= '2004-10-26 08:55:08';

SELECT count(*) FROM timestamptmp WHERE a  = '2004-10-26 08:55:08';

SELECT count(*) FROM timestamptmp WHERE a >= '2004-10-26 08:55:08';

SELECT count(*) FROM timestamptmp WHERE a >  '2004-10-26 08:55:08';

SELECT a, a <-> '2004-10-26 08:55:08' FROM timestamptmp ORDER BY a <-> '2004-10-26 08:55:08' LIMIT 3;

CREATE INDEX timestampidx ON timestamptmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM timestamptmp WHERE a <  '2004-10-26 08:55:08'::timestamp;

SELECT count(*) FROM timestamptmp WHERE a <= '2004-10-26 08:55:08'::timestamp;

SELECT count(*) FROM timestamptmp WHERE a  = '2004-10-26 08:55:08'::timestamp;

SELECT count(*) FROM timestamptmp WHERE a >= '2004-10-26 08:55:08'::timestamp;

SELECT count(*) FROM timestamptmp WHERE a >  '2004-10-26 08:55:08'::timestamp;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '2004-10-26 08:55:08' FROM timestamptmp ORDER BY a <-> '2004-10-26 08:55:08' LIMIT 3;
SELECT a, a <-> '2004-10-26 08:55:08' FROM timestamptmp ORDER BY a <-> '2004-10-26 08:55:08' LIMIT 3;
