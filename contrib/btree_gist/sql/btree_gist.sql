--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i btree_gist.sql
\set ECHO all

create table inttmp (b int4);

\copy inttmp from 'data/test_btree.data'

create table tstmp ( t timestamp without time zone );

\copy tstmp from 'data/test_btree_ts.data'

-- without idx

select count(*) from inttmp where b <=10;

select count(*) from tstmp where t < '2001-05-29 08:33:09';

-- create idx

create index aaaidx on inttmp using gist ( b );

create index tsidx on tstmp using gist ( t );

--with idx

set enable_seqscan=off;

select count(*) from inttmp where b <=10;

select count(*) from tstmp where t < '2001-05-29 08:33:09';

