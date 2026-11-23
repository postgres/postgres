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

-- same-sign infinities are zero distance apart
SELECT a, a <-> 'infinity' FROM timestamptmp ORDER BY a <-> 'infinity' LIMIT 3;
SELECT a, a <-> '-infinity' FROM timestamptmp ORDER BY a <-> '-infinity' LIMIT 3;
-- finite timestamps too far apart to subtract (KNN distance must not error)
CREATE TABLE timestampfar (a timestamp);
CREATE INDEX timestampfaridx ON timestampfar USING gist (a);
INSERT INTO timestampfar VALUES ('4714-11-24 00:00:00 BC'), ('294276-12-31 23:59:59');
EXPLAIN (COSTS OFF)
SELECT a FROM timestampfar ORDER BY a <-> '294276-12-31 23:59:59' LIMIT 1;
SELECT a FROM timestampfar ORDER BY a <-> '294276-12-31 23:59:59' LIMIT 1;
