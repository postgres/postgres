CREATE EXTENSION tsearch2;

--tsvector
SELECT '1'::tsvector;
SELECT '1 '::tsvector;
SELECT ' 1'::tsvector;
SELECT ' 1 '::tsvector;
SELECT '1 2'::tsvector;
SELECT '''1 2'''::tsvector;
SELECT E'''1 \\''2'''::tsvector;
SELECT E'''1 \\''2''3'::tsvector;
SELECT E'''1 \\''2'' 3'::tsvector;
SELECT E'''1 \\''2'' '' 3'' 4 '::tsvector;
select '''w'':4A,3B,2C,1D,5 a:8';
select 'a:3A b:2a'::tsvector || 'ba:1234 a:1B';
select setweight('w:12B w:13* w:12,5,6 a:1,3* a:3 w asd:1dc asd zxc:81,567,222A'::tsvector, 'c');
select strip('w:12B w:13* w:12,5,6 a:1,3* a:3 w asd:1dc asd'::tsvector);


--tsquery
SELECT '1'::tsquery;
SELECT '1 '::tsquery;
SELECT ' 1'::tsquery;
SELECT ' 1 '::tsquery;
SELECT '''1 2'''::tsquery;
SELECT E'''1 \\''2'''::tsquery;
SELECT '!1'::tsquery;
SELECT '1|2'::tsquery;
SELECT '1|!2'::tsquery;
SELECT '!1|2'::tsquery;
SELECT '!1|!2'::tsquery;
SELECT '!(!1|!2)'::tsquery;
SELECT '!(!1|2)'::tsquery;
SELECT '!(1|!2)'::tsquery;
SELECT '!(1|2)'::tsquery;
SELECT '1&2'::tsquery;
SELECT '!1&2'::tsquery;
SELECT '1&!2'::tsquery;
SELECT '!1&!2'::tsquery;
SELECT '(1&2)'::tsquery;
SELECT '1&(2)'::tsquery;
SELECT '!(1)&2'::tsquery;
SELECT '!(1&2)'::tsquery;
SELECT '1|2&3'::tsquery;
SELECT '1|(2&3)'::tsquery;
SELECT '(1|2)&3'::tsquery;
SELECT '1|2&!3'::tsquery;
SELECT '1|!2&3'::tsquery;
SELECT '!1|2&3'::tsquery;
SELECT '!1|(2&3)'::tsquery;
SELECT '!(1|2)&3'::tsquery;
SELECT '(!1|2)&3'::tsquery;
SELECT '1|(2|(4|(5|6)))'::tsquery;
SELECT '1|2|4|5|6'::tsquery;
SELECT '1&(2&(4&(5&6)))'::tsquery;
SELECT '1&2&4&5&6'::tsquery;
SELECT '1&(2&(4&(5|6)))'::tsquery;
SELECT '1&(2&(4&(5|!6)))'::tsquery;
SELECT E'1&(''2''&('' 4''&(\\|5 | ''6 \\'' !|&'')))'::tsquery;
SELECT '''the wether'':dc & '' sKies '':BC & a:d b:a';

select 'a' < 'b & c'::tsquery;
select 'a' > 'b & c'::tsquery;
select 'a | f' < 'b & c'::tsquery;
select 'a | ff' < 'b & c'::tsquery;
select 'a | f | g' < 'b & c'::tsquery;

select numnode( 'new'::tsquery );
select numnode( 'new & york'::tsquery );
select numnode( 'new & york | qwery'::tsquery );

create table test_tsquery (txtkeyword text, txtsample text);
\set ECHO none
\copy test_tsquery from stdin
'New York'	new & york | big & apple | nyc
Moscow	moskva | moscow
'Sanct Peter'	Peterburg | peter | 'Sanct Peterburg'
'foo bar qq'	foo & (bar | qq) & city
\.
\set ECHO all

alter table test_tsquery add column keyword tsquery;
update test_tsquery set keyword = to_tsquery('english', txtkeyword);
alter table test_tsquery add column sample tsquery;
update test_tsquery set sample = to_tsquery('english', txtsample::text);

create unique index bt_tsq on test_tsquery (keyword);

select count(*) from test_tsquery where keyword <  'new & york';
select count(*) from test_tsquery where keyword <= 'new & york';
select count(*) from test_tsquery where keyword = 'new & york';
select count(*) from test_tsquery where keyword >= 'new & york';
select count(*) from test_tsquery where keyword >  'new & york';

set enable_seqscan=off;

select count(*) from test_tsquery where keyword <  'new & york';
select count(*) from test_tsquery where keyword <= 'new & york';
select count(*) from test_tsquery where keyword = 'new & york';
select count(*) from test_tsquery where keyword >= 'new & york';
select count(*) from test_tsquery where keyword >  'new & york';

set enable_seqscan=on;

select rewrite('foo & bar & qq & new & york',  'new & york'::tsquery, 'big & apple | nyc | new & york & city');

select rewrite('moscow', 'select keyword, sample from test_tsquery'::text );
select rewrite('moscow & hotel', 'select keyword, sample from test_tsquery'::text );
select rewrite('bar &  new & qq & foo & york', 'select keyword, sample from test_tsquery'::text );

select rewrite( ARRAY['moscow', keyword, sample] ) from test_tsquery;
select rewrite( ARRAY['moscow & hotel', keyword, sample] ) from test_tsquery;
select rewrite( ARRAY['bar &  new & qq & foo & york', keyword, sample] ) from test_tsquery;


select keyword from test_tsquery where keyword @> 'new';
select keyword from test_tsquery where keyword @> 'moscow';
select keyword from test_tsquery where keyword <@ 'new';
select keyword from test_tsquery where keyword <@ 'moscow';
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow') as query where keyword <@ query;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow & hotel') as query where keyword <@ query;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'bar &  new & qq & foo & york') as query where keyword <@ query;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow') as query where query @> keyword;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow & hotel') as query where query @> keyword;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'bar &  new & qq & foo & york') as query where query @> keyword;

create index qq on test_tsquery using gist (keyword gist_tp_tsquery_ops);
set enable_seqscan='off';

select keyword from test_tsquery where keyword @> 'new';
select keyword from test_tsquery where keyword @> 'moscow';
select keyword from test_tsquery where keyword <@ 'new';
select keyword from test_tsquery where keyword <@ 'moscow';
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow') as query where keyword <@ query;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow & hotel') as query where keyword <@ query;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'bar &  new & qq & foo & york') as query where keyword <@ query;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow') as query where query @> keyword;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow & hotel') as query where query @> keyword;
select rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'bar &  new & qq & foo & york') as query where query @> keyword;
set enable_seqscan='on';



select lexize('simple', 'ASD56 hsdkf');
select lexize('english_stem', 'SKIES Problems identity');

select * from token_type('default');
select * from parse('default', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty');

SELECT to_tsvector('english', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty');

SELECT length(to_tsvector('english', '345 qw'));

SELECT length(to_tsvector('english', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty'));


select to_tsquery('english', 'qwe & sKies ');
select to_tsquery('simple', 'qwe & sKies ');
select to_tsquery('english', '''the wether'':dc & ''           sKies '':BC ');
select to_tsquery('english', 'asd&(and|fghj)');
select to_tsquery('english', '(asd&and)|fghj');
select to_tsquery('english', '(asd&!and)|fghj');
select to_tsquery('english', '(the|and&(i&1))&fghj');

select plainto_tsquery('english', 'the and z 1))& fghj');
select plainto_tsquery('english', 'foo bar') && plainto_tsquery('english', 'asd');
select plainto_tsquery('english', 'foo bar') || plainto_tsquery('english', 'asd fg');
select plainto_tsquery('english', 'foo bar') || !!plainto_tsquery('english', 'asd fg');
select plainto_tsquery('english', 'foo bar') && 'asd | fg';

select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca';
select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:B';
select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:A';
select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:C';
select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:CB';

CREATE TABLE test_tsvector( t text, a tsvector );

\copy test_tsvector from 'data/test_tsearch.data'

SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';

create index wowidx on test_tsvector using gist (a);
set enable_seqscan=off;

SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';

select set_curcfg('english');

CREATE TRIGGER tsvectorupdate
BEFORE UPDATE OR INSERT ON test_tsvector
FOR EACH ROW EXECUTE PROCEDURE tsearch2(a, t);

SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');

INSERT INTO test_tsvector (t) VALUES ('345 qwerty');

SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');

UPDATE test_tsvector SET t = null WHERE t = '345 qwerty';

SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');

insert into test_tsvector (t) values ('345 qwerty copyright');
select count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');
select count(*) FROM test_tsvector WHERE a @@ to_tsquery('copyright');

select rank(' a:1 s:2C d g'::tsvector, 'a | s');
select rank(' a:1 s:2B d g'::tsvector, 'a | s');
select rank(' a:1 s:2 d g'::tsvector, 'a | s');
select rank(' a:1 s:2C d g'::tsvector, 'a & s');
select rank(' a:1 s:2B d g'::tsvector, 'a & s');
select rank(' a:1 s:2 d g'::tsvector, 'a & s');

insert into test_tsvector (t) values ('foo bar foo the over foo qq bar');
drop trigger tsvectorupdate on test_tsvector;
select * from stat('select a from test_tsvector') order by ndoc desc, nentry desc, word collate "C";
insert into test_tsvector values ('1', 'a:1a,2,3b b:5a,6a,7c,8');
insert into test_tsvector values ('1', 'a:1a,2,3c b:5a,6b,7c,8b');
select * from stat('select a from test_tsvector','a') order by ndoc desc, nentry desc, word collate "C";
select * from stat('select a from test_tsvector','b') order by ndoc desc, nentry desc, word collate "C";
select * from stat('select a from test_tsvector','c') order by ndoc desc, nentry desc, word collate "C";
select * from stat('select a from test_tsvector','d') order by ndoc desc, nentry desc, word collate "C";
select * from stat('select a from test_tsvector','ad') order by ndoc desc, nentry desc, word collate "C";

select to_tsquery('english', 'skies & books');

select rank_cd(to_tsvector('Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
'), to_tsquery('sea&thousand&years'));

select rank_cd(to_tsvector('Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
'), to_tsquery('granite&sea'));

select rank_cd(to_tsvector('Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
'), to_tsquery('sea'));

select headline('Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
', to_tsquery('sea&thousand&years'));

select headline('Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
', to_tsquery('granite&sea'));

select headline('Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
', to_tsquery('sea'));


select headline('
<html>
<!-- some comment -->
<body>
Sea view wow <u>foo bar</u> <i>qq</i>
<a href="http://www.google.com/foo.bar.html" target="_blank">YES &nbsp;</a>
ff-bg
<script>
       document.write(15);
</script>
</body>
</html>',
to_tsquery('sea&foo'), 'HighlightAll=true');
--check debug
select * from public.ts_debug('Tsearch module for PostgreSQL 7.3.3');

--check ordering
insert into test_tsvector values (null, null);
select a is null, a from test_tsvector order by a;

drop index wowidx;
create index wowidx on test_tsvector using gin (a);
set enable_seqscan=off;

SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';
