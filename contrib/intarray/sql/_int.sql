--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i _int.sql
\set ECHO all

select intset(1234);
select icount('{1234234,234234}');
select sort('{1234234,-30,234234}');
select sort('{1234234,-30,234234}','asc');
select sort('{1234234,-30,234234}','desc');
select sort_asc('{1234234,-30,234234}');
select sort_desc('{1234234,-30,234234}');
select uniq('{1234234,-30,-30,234234,-30}');
select uniq(sort_asc('{1234234,-30,-30,234234,-30}'));
select idx('{1234234,-30,-30,234234,-30}',-30);
select subarray('{1234234,-30,-30,234234,-30}',2,3);
select subarray('{1234234,-30,-30,234234,-30}',-1,1);
select subarray('{1234234,-30,-30,234234,-30}',0,-1);

select #'{1234234,234234}'::int[];
select '{123,623,445}'::int[] + 1245;
select '{123,623,445}'::int[] + 445;
select '{123,623,445}'::int[] + '{1245,87,445}';
select '{123,623,445}'::int[] - 623;
select '{123,623,445}'::int[] - '{1623,623}';
select '{123,623,445}'::int[] | 623;
select '{123,623,445}'::int[] | 1623;
select '{123,623,445}'::int[] | '{1623,623}';
select '{123,623,445}'::int[] & '{1623,623}';


--test query_int
select '1'::query_int;
select ' 1'::query_int;
select '1 '::query_int;
select ' 1 '::query_int;
select ' ! 1 '::query_int;
select '!1'::query_int;
select '1|2'::query_int;
select '1|!2'::query_int;
select '!1|2'::query_int;
select '!1|!2'::query_int;
select '!(!1|!2)'::query_int;
select '!(!1|2)'::query_int;
select '!(1|!2)'::query_int;
select '!(1|2)'::query_int;
select '1&2'::query_int;
select '!1&2'::query_int;
select '1&!2'::query_int;
select '!1&!2'::query_int;
select '(1&2)'::query_int;
select '1&(2)'::query_int;
select '!(1)&2'::query_int;
select '!(1&2)'::query_int;
select '1|2&3'::query_int;
select '1|(2&3)'::query_int;
select '(1|2)&3'::query_int;
select '1|2&!3'::query_int;
select '1|!2&3'::query_int;
select '!1|2&3'::query_int;
select '!1|(2&3)'::query_int;
select '!(1|2)&3'::query_int;
select '(!1|2)&3'::query_int;
select '1|(2|(4|(5|6)))'::query_int;
select '1|2|4|5|6'::query_int;
select '1&(2&(4&(5&6)))'::query_int;
select '1&2&4&5&6'::query_int;
select '1&(2&(4&(5|6)))'::query_int;
select '1&(2&(4&(5|!6)))'::query_int;


CREATE TABLE test__int( a int[] );

\copy test__int from 'data/test__int.data'

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @ '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @ '{20,23}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @ '{20,23}' or a @ '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';

CREATE INDEX text_idx on test__int using gist ( a gist__int_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @ '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @ '{20,23}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @ '{20,23}' or a @ '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';

drop index text_idx;
CREATE INDEX text_idx on test__int using gist ( a gist__intbig_ops );

SELECT count(*) from test__int WHERE a && '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23|50';
SELECT count(*) from test__int WHERE a @ '{23,50}';
SELECT count(*) from test__int WHERE a @@ '23&50';
SELECT count(*) from test__int WHERE a @ '{20,23}';
SELECT count(*) from test__int WHERE a @@ '50&68';
SELECT count(*) from test__int WHERE a @ '{20,23}' or a @ '{50,68}';
SELECT count(*) from test__int WHERE a @@ '(20&23)|(50&68)';

