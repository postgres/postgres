--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i tsearch.sql
\set ECHO all

--txtidx
SELECT '1'::txtidx;
SELECT '1 '::txtidx;
SELECT ' 1'::txtidx;
SELECT ' 1 '::txtidx;
SELECT '1 2'::txtidx;
SELECT '''1 2'''::txtidx;
SELECT '''1 \\''2'''::txtidx;
SELECT '''1 \\''2''3'::txtidx;
SELECT '''1 \\''2'' 3'::txtidx;
SELECT '''1 \\''2'' '' 3'' 4 '::txtidx;

--query_txt
SELECT '1'::query_txt;
SELECT '1 '::query_txt;
SELECT ' 1'::query_txt;
SELECT ' 1 '::query_txt;
SELECT '''1 2'''::query_txt;
SELECT '''1 \\''2'''::query_txt;
SELECT '!1'::query_txt;
SELECT '1|2'::query_txt;
SELECT '1|!2'::query_txt;
SELECT '!1|2'::query_txt;
SELECT '!1|!2'::query_txt;
SELECT '!(!1|!2)'::query_txt;
SELECT '!(!1|2)'::query_txt;
SELECT '!(1|!2)'::query_txt;
SELECT '!(1|2)'::query_txt;
SELECT '1&2'::query_txt;
SELECT '!1&2'::query_txt;
SELECT '1&!2'::query_txt;
SELECT '!1&!2'::query_txt;
SELECT '(1&2)'::query_txt;
SELECT '1&(2)'::query_txt;
SELECT '!(1)&2'::query_txt;
SELECT '!(1&2)'::query_txt;
SELECT '1|2&3'::query_txt;
SELECT '1|(2&3)'::query_txt;
SELECT '(1|2)&3'::query_txt;
SELECT '1|2&!3'::query_txt;
SELECT '1|!2&3'::query_txt;
SELECT '!1|2&3'::query_txt;
SELECT '!1|(2&3)'::query_txt;
SELECT '!(1|2)&3'::query_txt;
SELECT '(!1|2)&3'::query_txt;
SELECT '1|(2|(4|(5|6)))'::query_txt;
SELECT '1|2|4|5|6'::query_txt;
SELECT '1&(2&(4&(5&6)))'::query_txt;
SELECT '1&2&4&5&6'::query_txt;
SELECT '1&(2&(4&(5|6)))'::query_txt;
SELECT '1&(2&(4&(5|!6)))'::query_txt;
SELECT '1&(''2''&('' 4''&(\\|5 | ''6 \\'' !|&'')))'::query_txt;
SELECT '1'::mquery_txt;
SELECT '1 '::mquery_txt;
SELECT ' 1'::mquery_txt;
SELECT ' 1 '::mquery_txt;
SELECT '''1 2'''::mquery_txt;
SELECT '''1 \\''2'''::mquery_txt;
SELECT '!1'::mquery_txt;
SELECT '1|2'::mquery_txt;
SELECT '1|!2'::mquery_txt;
SELECT '!1|2'::mquery_txt;
SELECT '!1|!2'::mquery_txt;
SELECT '!(!1|!2)'::mquery_txt;
SELECT '!(!1|2)'::mquery_txt;
SELECT '!(1|!2)'::mquery_txt;
SELECT '!(1|2)'::mquery_txt;
SELECT '1&2'::mquery_txt;
SELECT '!1&2'::mquery_txt;
SELECT '1&!2'::mquery_txt;
SELECT '!1&!2'::mquery_txt;
SELECT '(1&2)'::mquery_txt;
SELECT '1&(2)'::mquery_txt;
SELECT '!(1)&2'::mquery_txt;
SELECT '!(1&2)'::mquery_txt;
SELECT '1|2&3'::mquery_txt;
SELECT '1|(2&3)'::mquery_txt;
SELECT '(1|2)&3'::mquery_txt;
SELECT '1|2&!3'::mquery_txt;
SELECT '1|!2&3'::mquery_txt;
SELECT '!1|2&3'::mquery_txt;
SELECT '!1|(2&3)'::mquery_txt;
SELECT '!(1|2)&3'::mquery_txt;
SELECT '(!1|2)&3'::mquery_txt;
SELECT '1|(2|(4|(5|6)))'::mquery_txt;
SELECT '1|2|4|5|6'::mquery_txt;
SELECT '1&(2&(4&(5&6)))'::mquery_txt;
SELECT '1&2&4&5&6'::mquery_txt;
SELECT '1&(2&(4&(5|6)))'::mquery_txt;
SELECT '1&(2&(4&(5|!6)))'::mquery_txt;
SELECT '1&(''2''&('' 4''&(\\|5 | ''6 \\'' !|&'')))'::mquery_txt;
SELECT 'querty-fgries | http://www.google.com/index.html | www.rambler.ru/index.shtml'::mquery_txt;

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

SELECT txt2txtidx('345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty');

SELECT txtidxsize(txt2txtidx('345 qw'));

SELECT txtidxsize(txt2txtidx('345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234 
<i <b> wow  < jqw <> qwerty'));

INSERT INTO test_txtidx (a) VALUES ('345 qwerty');

CREATE TRIGGER txtidxupdate
BEFORE UPDATE OR INSERT ON test_txtidx
FOR EACH ROW EXECUTE PROCEDURE tsearch(a, t);

INSERT INTO test_txtidx (t) VALUES ('345 qwerty');

SELECT count(*) FROM test_txtidx WHERE a @@ '345&qwerty';

SELECT count(*) FROM test_txtidx WHERE a ## '345&qwerty';

UPDATE test_txtidx SET t = null WHERE t = '345 qwerty';

SELECT count(*) FROM test_txtidx WHERE a ## '345&qwerty';

SELECT count(*) FROM test_txtidx WHERE a @@ '345&qwerty';

