--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i btree_gist.sql
\set ECHO all

CREATE TABLE inttmp (b int4);

\copy inttmp from 'data/test_btree.data'

CREATE TABLE tstmp ( t timestamp without time zone );

\copy tstmp from 'data/test_btree_ts.data'

-- without idx

SELECT count(*) FROM inttmp WHERE b <=10;

SELECT count(*) FROM tstmp WHERE t < '2001-05-29 08:33:09';

-- create idx

CREATE INDEX aaaidx ON inttmp USING gist ( b );

CREATE INDEX tsidx ON tstmp USING gist ( t );

--with idx

SET enable_seqscan=off;

SELECT count(*) FROM inttmp WHERE b <=10;

SELECT count(*) FROM tstmp WHERE t < '2001-05-29 08:33:09';

