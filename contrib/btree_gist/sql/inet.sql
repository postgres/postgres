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

