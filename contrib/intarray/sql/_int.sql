--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of _int.sql.
--
SET client_min_messages = warning;
\set ECHO none
\i _int.sql
\set ECHO all
RESET client_min_messages;

SELECT intset(1234);
SELECT icount('{1234234,234234}');
SELECT sort('{1234234,-30,234234}');
SELECT sort('{1234234,-30,234234}','asc');
SELECT sort('{1234234,-30,234234}','desc');
SELECT sort_asc('{1234234,-30,234234}');
SELECT sort_desc('{1234234,-30,234234}');
SELECT uniq('{1234234,-30,-30,234234,-30}');
SELECT uniq(sort_asc('{1234234,-30,-30,234234,-30}'));
SELECT idx('{1234234,-30,-30,234234,-30}',-30);
SELECT subarray('{1234234,-30,-30,234234,-30}',2,3);
SELECT subarray('{1234234,-30,-30,234234,-30}',-1,1);
SELECT subarray('{1234234,-30,-30,234234,-30}',0,-1);

SELECT #'{1234234,234234}'::int[];
SELECT '{123,623,445}'::int[] + 1245;
SELECT '{123,623,445}'::int[] + 445;
SELECT '{123,623,445}'::int[] + '{1245,87,445}';
SELECT '{123,623,445}'::int[] - 623;
SELECT '{123,623,445}'::int[] - '{1623,623}';
SELECT '{123,623,445}'::int[] | 623;
SELECT '{123,623,445}'::int[] | 1623;
SELECT '{123,623,445}'::int[] | '{1623,623}';
SELECT '{123,623,445}'::int[] & '{1623,623}';


--test query_int
SELECT '1'::query_int;
SELECT ' 1'::query_int;
SELECT '1 '::query_int;
SELECT ' 1 '::query_int;
SELECT ' ! 1 '::query_int;
SELECT '!1'::query_int;
SELECT '1|2'::query_int;
SELECT '1|!2'::query_int;
SELECT '!1|2'::query_int;
SELECT '!1|!2'::query_int;
SELECT '!(!1|!2)'::query_int;
SELECT '!(!1|2)'::query_int;
SELECT '!(1|!2)'::query_int;
SELECT '!(1|2)'::query_int;
SELECT '1&2'::query_int;
SELECT '!1&2'::query_int;
SELECT '1&!2'::query_int;
SELECT '!1&!2'::query_int;
SELECT '(1&2)'::query_int;
SELECT '1&(2)'::query_int;
SELECT '!(1)&2'::query_int;
SELECT '!(1&2)'::query_int;
SELECT '1|2&3'::query_int;
SELECT '1|(2&3)'::query_int;
SELECT '(1|2)&3'::query_int;
SELECT '1|2&!3'::query_int;
SELECT '1|!2&3'::query_int;
SELECT '!1|2&3'::query_int;
SELECT '!1|(2&3)'::query_int;
SELECT '!(1|2)&3'::query_int;
SELECT '(!1|2)&3'::query_int;
SELECT '1|(2|(4|(5|6)))'::query_int;
SELECT '1|2|4|5|6'::query_int;
SELECT '1&(2&(4&(5&6)))'::query_int;
SELECT '1&2&4&5&6'::query_int;
SELECT '1&(2&(4&(5|6)))'::query_int;
SELECT '1&(2&(4&(5|!6)))'::query_int;


CREATE TABLE test__int( a int[] );

\copy test__int from 'data/test__int.data'

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';

CREATE INDEX text_idx on test__int using gist ( a gist__int_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';

DROP INDEX text_idx;
CREATE INDEX text_idx on test__int using gist ( a gist__intbig_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';

DROP INDEX text_idx;
CREATE INDEX text_idx on test__int using gin ( a gin__int_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
