-- date check

CREATE TABLE datetmp (a date);

\copy datetmp from 'data/date.data'

SET enable_seqscan=on;

SELECT count(*) FROM datetmp WHERE a <  '2001-02-13';

SELECT count(*) FROM datetmp WHERE a <= '2001-02-13';

SELECT count(*) FROM datetmp WHERE a  = '2001-02-13';

SELECT count(*) FROM datetmp WHERE a >= '2001-02-13';

SELECT count(*) FROM datetmp WHERE a >  '2001-02-13';

CREATE INDEX dateidx ON datetmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM datetmp WHERE a <  '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a <= '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a  = '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a >= '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a >  '2001-02-13'::date;
