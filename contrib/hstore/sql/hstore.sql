CREATE EXTENSION hstore;

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

select e'\\=a=>q=w'::hstore;
select e'"=a"=>q\\=w'::hstore;
select e'"\\"a"=>q>w'::hstore;
select e'\\"a=>q"w'::hstore;

select ''::hstore;
select '	'::hstore;

-- -> operator

select 'aa=>b, c=>d , b=>16'::hstore->'c';
select 'aa=>b, c=>d , b=>16'::hstore->'b';
select 'aa=>b, c=>d , b=>16'::hstore->'aa';
select ('aa=>b, c=>d , b=>16'::hstore->'gg') is null;
select ('aa=>NULL, c=>d , b=>16'::hstore->'aa') is null;
select ('aa=>"NULL", c=>d , b=>16'::hstore->'aa') is null;

-- -> array operator

select 'aa=>"NULL", c=>d , b=>16'::hstore -> ARRAY['aa','c'];
select 'aa=>"NULL", c=>d , b=>16'::hstore -> ARRAY['c','aa'];
select 'aa=>NULL, c=>d , b=>16'::hstore -> ARRAY['aa','c',null];
select 'aa=>1, c=>3, b=>2, d=>4'::hstore -> ARRAY[['b','d'],['aa','c']];

-- exists/defined

select exist('a=>NULL, b=>qq', 'a');
select exist('a=>NULL, b=>qq', 'b');
select exist('a=>NULL, b=>qq', 'c');
select exist('a=>"NULL", b=>qq', 'a');
select defined('a=>NULL, b=>qq', 'a');
select defined('a=>NULL, b=>qq', 'b');
select defined('a=>NULL, b=>qq', 'c');
select defined('a=>"NULL", b=>qq', 'a');
select hstore 'a=>NULL, b=>qq' ? 'a';
select hstore 'a=>NULL, b=>qq' ? 'b';
select hstore 'a=>NULL, b=>qq' ? 'c';
select hstore 'a=>"NULL", b=>qq' ? 'a';
select hstore 'a=>NULL, b=>qq' ?| ARRAY['a','b'];
select hstore 'a=>NULL, b=>qq' ?| ARRAY['b','a'];
select hstore 'a=>NULL, b=>qq' ?| ARRAY['c','a'];
select hstore 'a=>NULL, b=>qq' ?| ARRAY['c','d'];
select hstore 'a=>NULL, b=>qq' ?| '{}'::text[];
select hstore 'a=>NULL, b=>qq' ?& ARRAY['a','b'];
select hstore 'a=>NULL, b=>qq' ?& ARRAY['b','a'];
select hstore 'a=>NULL, b=>qq' ?& ARRAY['c','a'];
select hstore 'a=>NULL, b=>qq' ?& ARRAY['c','d'];
select hstore 'a=>NULL, b=>qq' ?& '{}'::text[];

-- delete

select delete('a=>1 , b=>2, c=>3'::hstore, 'a');
select delete('a=>null , b=>2, c=>3'::hstore, 'a');
select delete('a=>1 , b=>2, c=>3'::hstore, 'b');
select delete('a=>1 , b=>2, c=>3'::hstore, 'c');
select delete('a=>1 , b=>2, c=>3'::hstore, 'd');
select 'a=>1 , b=>2, c=>3'::hstore - 'a'::text;
select 'a=>null , b=>2, c=>3'::hstore - 'a'::text;
select 'a=>1 , b=>2, c=>3'::hstore - 'b'::text;
select 'a=>1 , b=>2, c=>3'::hstore - 'c'::text;
select 'a=>1 , b=>2, c=>3'::hstore - 'd'::text;
select pg_column_size('a=>1 , b=>2, c=>3'::hstore - 'b'::text)
         = pg_column_size('a=>1, b=>2'::hstore);

-- delete (array)

select delete('a=>1 , b=>2, c=>3'::hstore, ARRAY['d','e']);
select delete('a=>1 , b=>2, c=>3'::hstore, ARRAY['d','b']);
select delete('a=>1 , b=>2, c=>3'::hstore, ARRAY['a','c']);
select delete('a=>1 , b=>2, c=>3'::hstore, ARRAY[['b'],['c'],['a']]);
select delete('a=>1 , b=>2, c=>3'::hstore, '{}'::text[]);
select 'a=>1 , b=>2, c=>3'::hstore - ARRAY['d','e'];
select 'a=>1 , b=>2, c=>3'::hstore - ARRAY['d','b'];
select 'a=>1 , b=>2, c=>3'::hstore - ARRAY['a','c'];
select 'a=>1 , b=>2, c=>3'::hstore - ARRAY[['b'],['c'],['a']];
select 'a=>1 , b=>2, c=>3'::hstore - '{}'::text[];
select pg_column_size('a=>1 , b=>2, c=>3'::hstore - ARRAY['a','c'])
         = pg_column_size('b=>2'::hstore);
select pg_column_size('a=>1 , b=>2, c=>3'::hstore - '{}'::text[])
         = pg_column_size('a=>1, b=>2, c=>3'::hstore);

-- delete (hstore)

select delete('aa=>1 , b=>2, c=>3'::hstore, 'aa=>4, b=>2'::hstore);
select delete('aa=>1 , b=>2, c=>3'::hstore, 'aa=>NULL, c=>3'::hstore);
select delete('aa=>1 , b=>2, c=>3'::hstore, 'aa=>1, b=>2, c=>3'::hstore);
select delete('aa=>1 , b=>2, c=>3'::hstore, 'b=>2'::hstore);
select delete('aa=>1 , b=>2, c=>3'::hstore, ''::hstore);
select 'aa=>1 , b=>2, c=>3'::hstore - 'aa=>4, b=>2'::hstore;
select 'aa=>1 , b=>2, c=>3'::hstore - 'aa=>NULL, c=>3'::hstore;
select 'aa=>1 , b=>2, c=>3'::hstore - 'aa=>1, b=>2, c=>3'::hstore;
select 'aa=>1 , b=>2, c=>3'::hstore - 'b=>2'::hstore;
select 'aa=>1 , b=>2, c=>3'::hstore - ''::hstore;
select pg_column_size('a=>1 , b=>2, c=>3'::hstore - 'b=>2'::hstore)
         = pg_column_size('a=>1, c=>3'::hstore);
select pg_column_size('a=>1 , b=>2, c=>3'::hstore - ''::hstore)
         = pg_column_size('a=>1, b=>2, c=>3'::hstore);

-- ||
select 'aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f';
select 'aa=>1 , b=>2, cq=>3'::hstore || 'aq=>l';
select 'aa=>1 , b=>2, cq=>3'::hstore || 'aa=>l';
select 'aa=>1 , b=>2, cq=>3'::hstore || '';
select ''::hstore || 'cq=>l, b=>g, fg=>f';
select pg_column_size(''::hstore || ''::hstore) = pg_column_size(''::hstore);
select pg_column_size('aa=>1'::hstore || 'b=>2'::hstore)
         = pg_column_size('aa=>1, b=>2'::hstore);
select pg_column_size('aa=>1, b=>2'::hstore || ''::hstore)
         = pg_column_size('aa=>1, b=>2'::hstore);
select pg_column_size(''::hstore || 'aa=>1, b=>2'::hstore)
         = pg_column_size('aa=>1, b=>2'::hstore);

-- hstore(text,text)
select 'a=>g, b=>c'::hstore || hstore('asd', 'gf');
select 'a=>g, b=>c'::hstore || hstore('b', 'gf');
select 'a=>g, b=>c'::hstore || hstore('b', 'NULL');
select 'a=>g, b=>c'::hstore || hstore('b', NULL);
select ('a=>g, b=>c'::hstore || hstore(NULL, 'b')) is null;
select pg_column_size(hstore('b', 'gf'))
         = pg_column_size('b=>gf'::hstore);
select pg_column_size('a=>g, b=>c'::hstore || hstore('b', 'gf'))
         = pg_column_size('a=>g, b=>gf'::hstore);

-- slice()
select slice(hstore 'aa=>1, b=>2, c=>3', ARRAY['g','h','i']);
select slice(hstore 'aa=>1, b=>2, c=>3', ARRAY['c','b']);
select slice(hstore 'aa=>1, b=>2, c=>3', ARRAY['aa','b']);
select slice(hstore 'aa=>1, b=>2, c=>3', ARRAY['c','b','aa']);
select pg_column_size(slice(hstore 'aa=>1, b=>2, c=>3', ARRAY['c','b']))
         = pg_column_size('b=>2, c=>3'::hstore);
select pg_column_size(slice(hstore 'aa=>1, b=>2, c=>3', ARRAY['c','b','aa']))
         = pg_column_size('aa=>1, b=>2, c=>3'::hstore);

-- array input
select '{}'::text[]::hstore;
select ARRAY['a','g','b','h','asd']::hstore;
select ARRAY['a','g','b','h','asd','i']::hstore;
select ARRAY[['a','g'],['b','h'],['asd','i']]::hstore;
select ARRAY[['a','g','b'],['h','asd','i']]::hstore;
select ARRAY[[['a','g'],['b','h'],['asd','i']]]::hstore;
select hstore('{}'::text[]);
select hstore(ARRAY['a','g','b','h','asd']);
select hstore(ARRAY['a','g','b','h','asd','i']);
select hstore(ARRAY[['a','g'],['b','h'],['asd','i']]);
select hstore(ARRAY[['a','g','b'],['h','asd','i']]);
select hstore(ARRAY[[['a','g'],['b','h'],['asd','i']]]);
select hstore('[0:5]={a,g,b,h,asd,i}'::text[]);
select hstore('[0:2][1:2]={{a,g},{b,h},{asd,i}}'::text[]);

-- pairs of arrays
select hstore(ARRAY['a','b','asd'], ARRAY['g','h','i']);
select hstore(ARRAY['a','b','asd'], ARRAY['g','h',NULL]);
select hstore(ARRAY['z','y','x'], ARRAY['1','2','3']);
select hstore(ARRAY['aaa','bb','c','d'], ARRAY[null::text,null,null,null]);
select hstore(ARRAY['aaa','bb','c','d'], null);
select quote_literal(hstore('{}'::text[], '{}'::text[]));
select quote_literal(hstore('{}'::text[], null));
select hstore(ARRAY['a'], '{}'::text[]);  -- error
select hstore('{}'::text[], ARRAY['a']);  -- error
select pg_column_size(hstore(ARRAY['a','b','asd'], ARRAY['g','h','i']))
         = pg_column_size('a=>g, b=>h, asd=>i'::hstore);

-- records
select hstore(v) from (values (1, 'foo', 1.2, 3::float8)) v(a,b,c,d);
create domain hstestdom1 as integer not null default 0;
create table testhstore0 (a integer, b text, c numeric, d float8);
create table testhstore1 (a integer, b text, c numeric, d float8, e hstestdom1);
insert into testhstore0 values (1, 'foo', 1.2, 3::float8);
insert into testhstore1 values (1, 'foo', 1.2, 3::float8);
select hstore(v) from testhstore1 v;
select hstore(null::testhstore0);
select hstore(null::testhstore1);
select pg_column_size(hstore(v))
         = pg_column_size('a=>1, b=>"foo", c=>"1.2", d=>"3", e=>"0"'::hstore)
  from testhstore1 v;
select populate_record(v, hstore('c', '3.45')) from testhstore1 v;
select populate_record(v, hstore('d', '3.45')) from testhstore1 v;
select populate_record(v, hstore('e', '123')) from testhstore1 v;
select populate_record(v, hstore('e', null)) from testhstore1 v;
select populate_record(v, hstore('c', null)) from testhstore1 v;
select populate_record(v, hstore('b', 'foo') || hstore('a', '123')) from testhstore1 v;
select populate_record(v, hstore('b', 'foo') || hstore('e', null)) from testhstore0 v;
select populate_record(v, hstore('b', 'foo') || hstore('e', null)) from testhstore1 v;
select populate_record(v, '') from testhstore0 v;
select populate_record(v, '') from testhstore1 v;
select populate_record(null::testhstore1, hstore('c', '3.45') || hstore('a', '123'));
select populate_record(null::testhstore1, hstore('c', '3.45') || hstore('e', '123'));
select populate_record(null::testhstore0, '');
select populate_record(null::testhstore1, '');
select v #= hstore('c', '3.45') from testhstore1 v;
select v #= hstore('d', '3.45') from testhstore1 v;
select v #= hstore('e', '123') from testhstore1 v;
select v #= hstore('c', null) from testhstore1 v;
select v #= hstore('e', null) from testhstore0 v;
select v #= hstore('e', null) from testhstore1 v;
select v #= (hstore('b', 'foo') || hstore('a', '123')) from testhstore1 v;
select v #= (hstore('b', 'foo') || hstore('e', '123')) from testhstore1 v;
select v #= hstore '' from testhstore0 v;
select v #= hstore '' from testhstore1 v;
select null::testhstore1 #= (hstore('c', '3.45') || hstore('a', '123'));
select null::testhstore1 #= (hstore('c', '3.45') || hstore('e', '123'));
select null::testhstore0 #= hstore '';
select null::testhstore1 #= hstore '';
select v #= h from testhstore1 v, (values (hstore 'a=>123',1),('b=>foo,c=>3.21',2),('a=>null',3),('e=>123',4),('f=>blah',5)) x(h,i) order by i;

-- keys/values
select akeys('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f');
select akeys('""=>1');
select akeys('');
select avals('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f');
select avals('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>NULL');
select avals('""=>1');
select avals('');

select hstore_to_array('aa=>1, cq=>l, b=>g, fg=>NULL'::hstore);
select %% 'aa=>1, cq=>l, b=>g, fg=>NULL';

select hstore_to_matrix('aa=>1, cq=>l, b=>g, fg=>NULL'::hstore);
select %# 'aa=>1, cq=>l, b=>g, fg=>NULL';

select * from skeys('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f');
select * from skeys('""=>1');
select * from skeys('');
select * from svals('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>f');
select *, svals is null from svals('aa=>1 , b=>2, cq=>3'::hstore || 'cq=>l, b=>g, fg=>NULL');
select * from svals('""=>1');
select * from svals('');

select * from each('aaa=>bq, b=>NULL, ""=>1 ');

-- @>
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>b';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>b, c=>NULL';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>b, g=>NULL';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'g=>NULL';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>c';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>b';
select 'a=>b, b=>1, c=>NULL'::hstore @> 'a=>b, c=>q';

CREATE TABLE testhstore (h hstore);
\copy testhstore from 'data/hstore.data'

select count(*) from testhstore where h @> 'wait=>NULL';
select count(*) from testhstore where h @> 'wait=>CC';
select count(*) from testhstore where h @> 'wait=>CC, public=>t';
select count(*) from testhstore where h ? 'public';
select count(*) from testhstore where h ?| ARRAY['public','disabled'];
select count(*) from testhstore where h ?& ARRAY['public','disabled'];

create index hidx on testhstore using gist(h);
set enable_seqscan=off;

select count(*) from testhstore where h @> 'wait=>NULL';
select count(*) from testhstore where h @> 'wait=>CC';
select count(*) from testhstore where h @> 'wait=>CC, public=>t';
select count(*) from testhstore where h ? 'public';
select count(*) from testhstore where h ?| ARRAY['public','disabled'];
select count(*) from testhstore where h ?& ARRAY['public','disabled'];

drop index hidx;
create index hidx on testhstore using gin (h);
set enable_seqscan=off;

select count(*) from testhstore where h @> 'wait=>NULL';
select count(*) from testhstore where h @> 'wait=>CC';
select count(*) from testhstore where h @> 'wait=>CC, public=>t';
select count(*) from testhstore where h ? 'public';
select count(*) from testhstore where h ?| ARRAY['public','disabled'];
select count(*) from testhstore where h ?& ARRAY['public','disabled'];

select count(*) from (select (each(h)).key from testhstore) as wow ;
select key, count(*) from (select (each(h)).key from testhstore) as wow group by key order by count desc, key;

-- sort/hash
select count(distinct h) from testhstore;
set enable_hashagg = false;
select count(*) from (select h from (select * from testhstore union all select * from testhstore) hs group by h) hs2;
set enable_hashagg = true;
set enable_sort = false;
select count(*) from (select h from (select * from testhstore union all select * from testhstore) hs group by h) hs2;
select distinct * from (values (hstore '' || ''),('')) v(h);
set enable_sort = true;

-- btree
drop index hidx;
create index hidx on testhstore using btree (h);
set enable_seqscan=off;

select count(*) from testhstore where h #># 'p=>1';
select count(*) from testhstore where h = 'pos=>98, line=>371, node=>CBA, indexed=>t';

-- json and jsonb
select hstore_to_json('"a key" =>1, b => t, c => null, d=> 12345, e => 012345, f=> 1.234, g=> 2.345e+4');
select cast( hstore  '"a key" =>1, b => t, c => null, d=> 12345, e => 012345, f=> 1.234, g=> 2.345e+4' as json);
select hstore_to_json_loose('"a key" =>1, b => t, c => null, d=> 12345, e => 012345, f=> 1.234, g=> 2.345e+4');

select hstore_to_jsonb('"a key" =>1, b => t, c => null, d=> 12345, e => 012345, f=> 1.234, g=> 2.345e+4');
select cast( hstore  '"a key" =>1, b => t, c => null, d=> 12345, e => 012345, f=> 1.234, g=> 2.345e+4' as jsonb);
select hstore_to_jsonb_loose('"a key" =>1, b => t, c => null, d=> 12345, e => 012345, f=> 1.234, g=> 2.345e+4');

create table test_json_agg (f1 text, f2 hstore);
insert into test_json_agg values ('rec1','"a key" =>1, b => t, c => null, d=> 12345, e => 012345, f=> 1.234, g=> 2.345e+4'),
       ('rec2','"a key" =>2, b => f, c => "null", d=> -12345, e => 012345.6, f=> -1.234, g=> 0.345e-4');
select json_agg(q) from test_json_agg q;
select json_agg(q) from (select f1, hstore_to_json_loose(f2) as f2 from test_json_agg) q;
