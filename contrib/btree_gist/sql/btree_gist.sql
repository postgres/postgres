--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of btree_gist.sql.
--
\set ECHO none
\i btree_gist.sql
\set ECHO all

CREATE TABLE int4tmp (b int4);

\copy int4tmp from 'data/test_btree.data'

CREATE TABLE int8tmp (b int8);

\copy int8tmp from 'data/test_btree.data'

CREATE TABLE float4tmp (b float4);

\copy float4tmp from 'data/test_btree.data'

CREATE TABLE float8tmp (b float8);

\copy float8tmp from 'data/test_btree.data'

CREATE TABLE tstmp ( t timestamp without time zone );

\copy tstmp from 'data/test_btree_ts.data'

-- without idx

SELECT count(*) FROM int4tmp WHERE b <=10;

SELECT count(*) FROM int8tmp WHERE b <=10;

SELECT count(*) FROM float4tmp WHERE b <=10;

SELECT count(*) FROM float8tmp WHERE b <=10;

SELECT count(*) FROM tstmp WHERE t < '2001-05-29 08:33:09';

-- create idx

CREATE INDEX aaaidx ON int4tmp USING gist ( b );

CREATE INDEX bbbidx ON int8tmp USING gist ( b );

CREATE INDEX cccidx ON float4tmp USING gist ( b );

CREATE INDEX dddidx ON float8tmp USING gist ( b );

CREATE INDEX tsidx ON tstmp USING gist ( t );

--with idx

SET enable_seqscan=off;

SELECT count(*) FROM int4tmp WHERE b <=10::int4;

SELECT count(*) FROM int8tmp WHERE b <=10::int8;

SELECT count(*) FROM float4tmp WHERE b <=10::float4;

SELECT count(*) FROM float8tmp WHERE b <=10::float8;

SELECT count(*) FROM tstmp WHERE t < '2001-05-29 08:33:09';

