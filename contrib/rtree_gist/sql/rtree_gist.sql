--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i rtree_gist.sql
\set ECHO all

CREATE TABLE boxtmp (b box);

\copy boxtmp from 'data/test_box.data'

SELECT count(*)
FROM boxtmp
WHERE b && '(1000,1000,0,0)'::box;

CREATE INDEX bix ON boxtmp USING rtree (b);

SELECT count(*)
FROM boxtmp
WHERE b && '(1000,1000,0,0)'::box;

DROP INDEX bix;

CREATE INDEX bix ON boxtmp USING gist (b);

SELECT count(*)
FROM boxtmp
WHERE b && '(1000,1000,0,0)'::box;

CREATE TABLE polytmp (p polygon);

\copy polytmp from 'data/test_box.data'

CREATE INDEX pix ON polytmp USING rtree (p);

SELECT count(*)
FROM polytmp
WHERE p && '(1000,1000),(0,0)'::polygon;

DROP INDEX pix;

CREATE INDEX pix ON polytmp USING gist (p);

SELECT count(*)
FROM polytmp
WHERE p && '(1000,1000),(0,0)'::polygon;
