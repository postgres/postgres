--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i _int.sql
\set ECHO all

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

