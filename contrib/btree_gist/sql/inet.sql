-- inet check

CREATE TABLE inettmp (a inet);

\copy inettmp from 'data/inet.data'

SET enable_seqscan=on;

SELECT count(*) FROM inettmp WHERE a <  '89.225.196.191';

SELECT count(*) FROM inettmp WHERE a <= '89.225.196.191';

SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191';

SELECT count(*) FROM inettmp WHERE a >= '89.225.196.191';

SELECT count(*) FROM inettmp WHERE a >  '89.225.196.191';

CREATE INDEX inetidx ON inettmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM inettmp WHERE a <  '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a <= '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a >= '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a >  '89.225.196.191'::inet;

VACUUM ANALYZE inettmp;

-- gist_inet_ops lacks a fetch function, so this should not be index-only scan
EXPLAIN (COSTS OFF)
SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;

DROP INDEX inetidx;

CREATE INDEX ON inettmp USING gist (a gist_inet_ops, a inet_ops);

-- this can be an index-only scan, as long as the planner uses the right column
EXPLAIN (COSTS OFF)
SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;

SELECT count(*) FROM inettmp WHERE a  = '89.225.196.191'::inet;
