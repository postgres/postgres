--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i _int.sql
\set ECHO all

CREATE TABLE test__int( a int[] );

\copy test__int from 'data/test__int.data'

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @ '{23,50}';

CREATE INDEX text_idx on test__int using gist ( a gist__int_ops ) with ( islossy );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @ '{23,50}';

drop index text_idx;
CREATE INDEX text_idx on test__int using gist ( a gist__intbig_ops ) with ( islossy );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @ '{23,50}';

