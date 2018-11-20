-- money check

CREATE TABLE moneytmp (a money);

\copy moneytmp from 'data/cash.data'

SET enable_seqscan=on;

SELECT count(*) FROM moneytmp WHERE a <  '22649.64';

SELECT count(*) FROM moneytmp WHERE a <= '22649.64';

SELECT count(*) FROM moneytmp WHERE a  = '22649.64';

SELECT count(*) FROM moneytmp WHERE a >= '22649.64';

SELECT count(*) FROM moneytmp WHERE a >  '22649.64';

SELECT a, a <-> '21472.79' FROM moneytmp ORDER BY a <-> '21472.79' LIMIT 3;

CREATE INDEX moneyidx ON moneytmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM moneytmp WHERE a <  '22649.64'::money;

SELECT count(*) FROM moneytmp WHERE a <= '22649.64'::money;

SELECT count(*) FROM moneytmp WHERE a  = '22649.64'::money;

SELECT count(*) FROM moneytmp WHERE a >= '22649.64'::money;

SELECT count(*) FROM moneytmp WHERE a >  '22649.64'::money;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '21472.79' FROM moneytmp ORDER BY a <-> '21472.79' LIMIT 3;
SELECT a, a <-> '21472.79' FROM moneytmp ORDER BY a <-> '21472.79' LIMIT 3;
