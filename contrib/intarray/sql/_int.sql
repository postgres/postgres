CREATE EXTENSION intarray;

-- Check whether any of our opclasses fail amvalidate
SELECT amname, opcname
FROM pg_opclass opc LEFT JOIN pg_am am ON am.oid = opcmethod
WHERE opc.oid >= 16384 AND NOT amvalidate(opc.oid);

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
SELECT '{-1,3,1}'::int[] & '{1,2}';
SELECT '{1}'::int[] & '{2}'::int[];
SELECT array_dims('{1}'::int[] & '{2}'::int[]);
SELECT ('{1}'::int[] & '{2}'::int[]) = '{}'::int[];
SELECT ('{}'::int[] & '{}'::int[]) = '{}'::int[];


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
ANALYZE test__int;

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';

SET enable_seqscan = off;  -- not all of these would use index by default

CREATE INDEX text_idx on test__int using gist ( a gist__int_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';

INSERT INTO test__int SELECT array(SELECT x FROM generate_series(1, 1001) x); -- should fail

DROP INDEX text_idx;
CREATE INDEX text_idx on test__int using gist ( a gist__intbig_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';

DROP INDEX text_idx;
CREATE INDEX text_idx on test__int using gin ( a gin__int_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @> '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @> '{20,23}';
SELECT count(*) from test__int WHERE a <@ '{73,23,20}';
SELECT count(*) from test__int WHERE a = '{73,23,20}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @> '{20,23}' or a @> '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';
SELECT count(*) from test__int WHERE a @@ '20 | !21';
SELECT count(*) from test__int WHERE a @@ '!20 & !21';

RESET enable_seqscan;
