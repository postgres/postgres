--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i tsearch.sql
\set ECHO all

--txtidx
select '1'::txtidx;
select '1 '::txtidx;
select ' 1'::txtidx;
select ' 1 '::txtidx;
select '1 2'::txtidx;
select '\'1 2\''::txtidx;
select '\'1 \\\'2\''::txtidx;
select '\'1 \\\'2\'3'::txtidx;
select '\'1 \\\'2\' 3'::txtidx;
select '\'1 \\\'2\' \' 3\' 4 '::txtidx;

--query_txt
select '1'::query_txt;
select '1 '::query_txt;
select ' 1'::query_txt;
select ' 1 '::query_txt;
select '\'1 2\''::query_txt;
select '\'1 \\\'2\''::query_txt;
select '!1'::query_txt;
select '1|2'::query_txt;
select '1|!2'::query_txt;
select '!1|2'::query_txt;
select '!1|!2'::query_txt;
select '!(!1|!2)'::query_txt;
select '!(!1|2)'::query_txt;
select '!(1|!2)'::query_txt;
select '!(1|2)'::query_txt;
select '1&2'::query_txt;
select '!1&2'::query_txt;
select '1&!2'::query_txt;
select '!1&!2'::query_txt;
select '(1&2)'::query_txt;
select '1&(2)'::query_txt;
select '!(1)&2'::query_txt;
select '!(1&2)'::query_txt;
select '1|2&3'::query_txt;
select '1|(2&3)'::query_txt;
select '(1|2)&3'::query_txt;
select '1|2&!3'::query_txt;
select '1|!2&3'::query_txt;
select '!1|2&3'::query_txt;
select '!1|(2&3)'::query_txt;
select '!(1|2)&3'::query_txt;
select '(!1|2)&3'::query_txt;
select '1|(2|(4|(5|6)))'::query_txt;
select '1|2|4|5|6'::query_txt;
select '1&(2&(4&(5&6)))'::query_txt;
select '1&2&4&5&6'::query_txt;
select '1&(2&(4&(5|6)))'::query_txt;
select '1&(2&(4&(5|!6)))'::query_txt;
select '1&(\'2\'&(\' 4\'&(\\|5 | \'6 \\\' !|&\')))'::query_txt;
select '1'::mquery_txt;
select '1 '::mquery_txt;
select ' 1'::mquery_txt;
select ' 1 '::mquery_txt;
select '\'1 2\''::mquery_txt;
select '\'1 \\\'2\''::mquery_txt;
select '!1'::mquery_txt;
select '1|2'::mquery_txt;
select '1|!2'::mquery_txt;
select '!1|2'::mquery_txt;
select '!1|!2'::mquery_txt;
select '!(!1|!2)'::mquery_txt;
select '!(!1|2)'::mquery_txt;
select '!(1|!2)'::mquery_txt;
select '!(1|2)'::mquery_txt;
select '1&2'::mquery_txt;
select '!1&2'::mquery_txt;
select '1&!2'::mquery_txt;
select '!1&!2'::mquery_txt;
select '(1&2)'::mquery_txt;
select '1&(2)'::mquery_txt;
select '!(1)&2'::mquery_txt;
select '!(1&2)'::mquery_txt;
select '1|2&3'::mquery_txt;
select '1|(2&3)'::mquery_txt;
select '(1|2)&3'::mquery_txt;
select '1|2&!3'::mquery_txt;
select '1|!2&3'::mquery_txt;
select '!1|2&3'::mquery_txt;
select '!1|(2&3)'::mquery_txt;
select '!(1|2)&3'::mquery_txt;
select '(!1|2)&3'::mquery_txt;
select '1|(2|(4|(5|6)))'::mquery_txt;
select '1|2|4|5|6'::mquery_txt;
select '1&(2&(4&(5&6)))'::mquery_txt;
select '1&2&4&5&6'::mquery_txt;
select '1&(2&(4&(5|6)))'::mquery_txt;
select '1&(2&(4&(5|!6)))'::mquery_txt;
select '1&(\'2\'&(\' 4\'&(\\|5 | \'6 \\\' !|&\')))'::mquery_txt;
select 'querty-fgries | http://www.google.com/index.html | www.rambler.ru/index.shtml'::mquery_txt;

CREATE TABLE test_txtidx( t text, a txtidx );

\copy test_txtidx from 'data/test_tsearch.data'

SELECT count(*) FROM test_txtidx WHERE a @@ 'wr|qh';
SELECT count(*) FROM test_txtidx WHERE a @@ 'wr&qh';
SELECT count(*) FROM test_txtidx WHERE a @@ 'eq&yt';
SELECT count(*) FROM test_txtidx WHERE a @@ 'eq|yt';
SELECT count(*) FROM test_txtidx WHERE a @@ '(eq&yt)|(wr&qh)';
SELECT count(*) FROM test_txtidx WHERE a @@ '(eq|yt)&(wr|qh)';

SELECT count(*) FROM test_txtidx WHERE a ## 'wR|qh';
SELECT count(*) FROM test_txtidx WHERE a ## 'wR&qh';
SELECT count(*) FROM test_txtidx WHERE a ## 'eq&yt';
SELECT count(*) FROM test_txtidx WHERE a ## 'eq|yt';
SELECT count(*) FROM test_txtidx WHERE a ## '(eq&yt)|(wR&qh)';
SELECT count(*) FROM test_txtidx WHERE a ## '(eq|yt)&(wR|qh)';

create index wowidx on test_txtidx using gist (a);

SELECT count(*) FROM test_txtidx WHERE a @@ 'wr|qh';
SELECT count(*) FROM test_txtidx WHERE a @@ 'wr&qh';
SELECT count(*) FROM test_txtidx WHERE a @@ 'eq&yt';
SELECT count(*) FROM test_txtidx WHERE a @@ 'eq|yt';
SELECT count(*) FROM test_txtidx WHERE a @@ '(eq&yt)|(wr&qh)';
SELECT count(*) FROM test_txtidx WHERE a @@ '(eq|yt)&(wr|qh)';

SELECT count(*) FROM test_txtidx WHERE a ## 'wR|qh';
SELECT count(*) FROM test_txtidx WHERE a ## 'wR&qh';
SELECT count(*) FROM test_txtidx WHERE a ## 'eq&yt';
SELECT count(*) FROM test_txtidx WHERE a ## 'eq|yt';
SELECT count(*) FROM test_txtidx WHERE a ## '(eq&yt)|(wR&qh)';
SELECT count(*) FROM test_txtidx WHERE a ## '(eq|yt)&(wR|qh)';

select txt2txtidx('345 qwe@efd.r \' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty');

select txtidxsize(txt2txtidx('345 qw'));

select txtidxsize(txt2txtidx('345 qwe@efd.r \' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty'));

insert into test_txtidx (a) values ('345 qwerty');

create trigger txtidxupdate before update or insert on test_txtidx
for each row execute procedure tsearch(a, t);

insert into test_txtidx (t) values ('345 qwerty');

select count(*) FROM test_txtidx WHERE a @@ '345&qwerty';

select count(*) FROM test_txtidx WHERE a ## '345&qwerty';

update test_txtidx set t = null where t = '345 qwerty';

select count(*) FROM test_txtidx WHERE a ## '345&qwerty';

select count(*) FROM test_txtidx WHERE a @@ '345&qwerty';

