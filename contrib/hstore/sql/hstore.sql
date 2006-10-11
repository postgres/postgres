--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of hstore.sql.
--
SET client_min_messages = warning;
\set ECHO none
\i hstore.sql
\set ECHO all
RESET client_min_messages;

set escape_string_warning=off;

--hstore;

select ''::hstore;
select 'a=>b'::hstore;
select ' a=>b'::hstore;
select 'a =>b'::hstore;
select 'a=>b '::hstore;
select 'a=> b'::hstore;
select '"a"=>"b"'::hstore;
select ' "a"=>"b"'::hstore;
select '"a" =>"b"'::hstore;
select '"a"=>"b" '::hstore;
select '"a"=> "b"'::hstore;
select 'aa=>bb'::hstore;
select ' aa=>bb'::hstore;
select 'aa =>bb'::hstore;
select 'aa=>bb '::hstore;
select 'aa=> bb'::hstore;
select '"aa"=>"bb"'::hstore;
select ' "aa"=>"bb"'::hstore;
select '"aa" =>"bb"'::hstore;
select '"aa"=>"bb" '::hstore;
select '"aa"=> "bb"'::hstore;

select 'aa=>bb, cc=>dd'::hstore;
select 'aa=>bb , cc=>dd'::hstore;
select 'aa=>bb ,cc=>dd'::hstore;
select 'aa=>bb, "cc"=>dd'::hstore;
select 'aa=>bb , "cc"=>dd'::hstore;
select 'aa=>bb ,"cc"=>dd'::hstore;
select 'aa=>"bb", cc=>dd'::hstore;
select 'aa=>"bb" , cc=>dd'::hstore;
select 'aa=>"bb" ,cc=>dd'::hstore;

select 'aa=>null'::hstore;
select 'aa=>NuLl'::hstore;
select 'aa=>"NuLl"'::hstore;

select '\\=a=>q=w'::hstore;
select '"=a"=>q\\=w'::hstore;
select '"\\"a"=>q>w'::hstore;
select '\\"a=>q"w'::hstore;

select ''::hstore;
select '	'::hstore;

-- -> operator

select 'aa=>b, c=>d , b=>16'::hstore->'c';
select 'aa=>b, c=>d , b=>16'::hstore->'b';
select 'aa=>b, c=>d , b=>16'::hstore->'aa';
select ('aa=>b, c=>d , b=>16'::hstore->'gg') is null;
select ('aa=>NULL, c=>d , b=>16'::hstore->'aa') is null;

-- exists/defined

select exist('a=>NULL, b=>qq', 'a');
select exist('a=>NULL, b=>qq', 'b');
select exist('a=>NULL, b=>qq', 'c');
select defined('a=>NULL, b=>qq', 'a');
select defined('a=>NULL, b=>qq', 'b');
select defined('a=>NULL, b=>qq', 'c');

-- delete 

select delete('a=>1 , b=>2, c=>3'::hstore, 'a');
select delete('a=>null , b=>2, c=>3'::hstore, 'a');
select delete('a=>1 , b=>2, c=>3'::hstore, 'b');
select delete('a=>1 , b=>2, c=>3'::hstore, 'c');
select delete('a=>1 , b=>2, c=>3'::hstore, 'd');

-- ||
select 'aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f';
select 'aa=>1 , b=>2, cq=>3'::hstore || 'aq=>l';
select 'aa=>1 , b=>2, cq=>3'::hstore || 'aa=>l';
select 'aa=>1 , b=>2, cq=>3'::hstore || '';
select ''::hstore || 'cq=>l, b=>g, fg=>f';

-- =>
select 'a=>g, b=>c'::hstore || ( 'asd'=>'gf' );
select 'a=>g, b=>c'::hstore || ( 'b'=>'gf' );

-- keys/values
select akeys('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f');
select akeys('""=>1');
select akeys('');
select avals('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f');
select avals('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>NULL');
select avals('""=>1');
select avals('');

select * from skeys('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f');
select * from skeys('""=>1');
select * from skeys('');
select * from svals('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f');
select *, svals is null from svals('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>NULL');
select * from svals('""=>1');
select * from svals('');

select * from each('aaa=>bq, b=>NULL, ""=>1 ');

-- @>
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>NULL';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>NULL, c=>NULL';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>NULL, g=>NULL';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'g=>NULL';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>c';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>b';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>b, c=>NULL';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>b, c=>q';

CREATE TABLE testhstore (h hstore);
\copy testhstore from 'data/hstore.data'

select count(*) from testhstore where h @> 'wait=>NULL';
select count(*) from testhstore where h @> 'wait=>CC';
select count(*) from testhstore where h @> 'wait=>CC, public=>t';

create index hidx on testhstore using gist(h);
set enable_seqscan=off;

select count(*) from testhstore where h @> 'wait=>NULL';
select count(*) from testhstore where h @> 'wait=>CC';
select count(*) from testhstore where h @> 'wait=>CC, public=>t';

select count(*) from (select (each(h)).key from testhstore) as wow ;
select key, count(*) from (select (each(h)).key from testhstore) as wow group by key order by count desc, key;
