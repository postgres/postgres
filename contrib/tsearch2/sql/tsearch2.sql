--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i tsearch2.sql
\set ECHO all

--tsvector
SELECT '1'::tsvector;
SELECT '1 '::tsvector;
SELECT ' 1'::tsvector;
SELECT ' 1 '::tsvector;
SELECT '1 2'::tsvector;
SELECT '''1 2'''::tsvector;
SELECT '''1 \\''2'''::tsvector;
SELECT '''1 \\''2''3'::tsvector;
SELECT '''1 \\''2'' 3'::tsvector;
SELECT '''1 \\''2'' '' 3'' 4 '::tsvector;
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
SELECT '''1 \\''2'''::tsquery;
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
SELECT '1&(''2''&('' 4''&(\\|5 | ''6 \\'' !|&'')))'::tsquery;
SELECT '''the wether'':dc & '' sKies '':BC & a:d b:a';

select lexize('simple', 'ASD56 hsdkf');
select lexize('en_stem', 'SKIES Problems identity');

select * from token_type('default');
select * from parse('default', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty');

SELECT to_tsvector('default', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty');

SELECT length(to_tsvector('default', '345 qw'));

SELECT length(to_tsvector('default', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty'));


select to_tsquery('default', 'qwe & sKies '); 
select to_tsquery('simple', 'qwe & sKies '); 
select to_tsquery('default', '''the wether'':dc & ''           sKies '':BC ');
select to_tsquery('default', 'asd&(and|fghj)');
select to_tsquery('default', '(asd&and)|fghj');
select to_tsquery('default', '(asd&!and)|fghj');
select to_tsquery('default', '(the|and&(i&1))&fghj');
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

select set_curcfg('default');

CREATE TRIGGER tsvectorupdate
BEFORE UPDATE OR INSERT ON test_tsvector
FOR EACH ROW EXECUTE PROCEDURE tsearch2(a, t);

SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');

INSERT INTO test_tsvector (t) VALUES ('345 qwerty');

SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');

UPDATE test_tsvector SET t = null WHERE t = '345 qwerty';

SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');

drop trigger tsvectorupdate on test_tsvector;
create function wow(text) returns text as 'select $1 || '' copyright''; ' language sql;
create trigger tsvectorupdate before update or insert on test_tsvector
for each row execute procedure tsearch2(a, wow, t);
insert into test_tsvector (t) values ('345 qwerty');
select count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');
select count(*) FROM test_tsvector WHERE a @@ to_tsquery('copyright');

select rank(' a:1 s:2C d g'::tsvector, 'a | s');
select rank(' a:1 s:2B d g'::tsvector, 'a | s');
select rank(' a:1 s:2 d g'::tsvector, 'a | s');
select rank(' a:1 s:2C d g'::tsvector, 'a & s');
select rank(' a:1 s:2B d g'::tsvector, 'a & s');
select rank(' a:1 s:2 d g'::tsvector, 'a & s');

insert into test_tsvector (t) values ('foo bar foo the over foo qq bar');
select * from stat('select a from test_tsvector') order by ndoc desc, nentry desc, word;

select reset_tsearch();
select to_tsquery('default', 'skies & books');

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

select get_covers(to_tsvector('Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
'), to_tsquery('sea&thousand&years'));

select get_covers(to_tsvector('Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
'), to_tsquery('granite&sea'));

select get_covers(to_tsvector('Erosion It took the sea a thousand years,
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

--check debug
select * from ts_debug('Tsearch module for PostgreSQL 7.3.3');
