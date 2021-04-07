-- interval check

CREATE TABLE intervaltmp (a interval);

\copy intervaltmp from 'data/interval.data'

SET enable_seqscan=on;

SELECT count(*) FROM intervaltmp WHERE a <  '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a <= '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a  = '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a >= '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a >  '199 days 21:21:23';

SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;

SET client_min_messages = DEBUG1;
CREATE INDEX intervalidx ON intervaltmp USING gist ( a );
CREATE INDEX intervalidx_b ON intervaltmp USING gist ( a ) WITH (buffering=on);
DROP INDEX intervalidx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM intervaltmp WHERE a <  '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a <= '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a  = '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a >= '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a >  '199 days 21:21:23'::interval;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;
SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;

SET enable_indexonlyscan=off;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;
SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;
