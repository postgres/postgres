-- macaddr check

CREATE TABLE macaddr8tmp (a macaddr8);

\copy macaddr8tmp from 'data/macaddr.data'

SET enable_seqscan=on;

SELECT count(*) FROM macaddr8tmp WHERE a <  '22:00:5c:e5:9b:0d';

SELECT count(*) FROM macaddr8tmp WHERE a <= '22:00:5c:e5:9b:0d';

SELECT count(*) FROM macaddr8tmp WHERE a  = '22:00:5c:e5:9b:0d';

SELECT count(*) FROM macaddr8tmp WHERE a >= '22:00:5c:e5:9b:0d';

SELECT count(*) FROM macaddr8tmp WHERE a >  '22:00:5c:e5:9b:0d';

SET client_min_messages = DEBUG1;
CREATE INDEX macaddr8idx ON macaddr8tmp USING gist ( a );
CREATE INDEX macaddr8idx_b ON macaddr8tmp USING gist ( a ) WITH (buffering=on);
DROP INDEX macaddr8idx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM macaddr8tmp WHERE a <  '22:00:5c:e5:9b:0d'::macaddr8;

SELECT count(*) FROM macaddr8tmp WHERE a <= '22:00:5c:e5:9b:0d'::macaddr8;

SELECT count(*) FROM macaddr8tmp WHERE a  = '22:00:5c:e5:9b:0d'::macaddr8;

SELECT count(*) FROM macaddr8tmp WHERE a >= '22:00:5c:e5:9b:0d'::macaddr8;

SELECT count(*) FROM macaddr8tmp WHERE a >  '22:00:5c:e5:9b:0d'::macaddr8;

-- Test index-only scans
SET enable_bitmapscan=off;
EXPLAIN (COSTS OFF)
SELECT * FROM macaddr8tmp WHERE a < '02:03:04:05:06:07'::macaddr8;
SELECT * FROM macaddr8tmp WHERE a < '02:03:04:05:06:07'::macaddr8;
