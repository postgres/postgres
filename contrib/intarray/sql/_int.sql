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

