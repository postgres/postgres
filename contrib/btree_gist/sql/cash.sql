-- money check

CREATE TABLE moneytmp (a money) WITH OIDS;

\copy moneytmp from 'data/cash.data'

SET enable_seqscan=on;

SELECT count(*) FROM moneytmp WHERE a <  '22649.64';

SELECT count(*) FROM moneytmp WHERE a <= '22649.64';

SELECT count(*) FROM moneytmp WHERE a  = '22649.64';

SELECT count(*) FROM moneytmp WHERE a >= '22649.64';

SELECT count(*) FROM moneytmp WHERE a >  '22649.64';

CREATE INDEX moneyidx ON moneytmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM moneytmp WHERE a <  '22649.64'::money;

SELECT count(*) FROM moneytmp WHERE a <= '22649.64'::money;

SELECT count(*) FROM moneytmp WHERE a  = '22649.64'::money;

SELECT count(*) FROM moneytmp WHERE a >= '22649.64'::money;

SELECT count(*) FROM moneytmp WHERE a >  '22649.64'::money;
