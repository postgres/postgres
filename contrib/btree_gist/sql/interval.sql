-- interval check

CREATE TABLE intervaltmp (a interval);

\copy intervaltmp from 'data/interval.data'

SET enable_seqscan=on;

SELECT count(*) FROM intervaltmp WHERE a <  '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a <= '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a  = '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a >= '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a >  '199 days 21:21:23';

CREATE INDEX intervalidx ON intervaltmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM intervaltmp WHERE a <  '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a <= '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a  = '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a >= '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a >  '199 days 21:21:23'::interval;

