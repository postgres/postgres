--
-- Sanity checks for text search catalogs
--
-- NB: we assume the oidjoins test will have caught any dangling links,
-- that is OID or REGPROC fields that are not zero and do not match some
-- row in the linked-to table.  However, if we want to enforce that a link
-- field can't be 0, we have to check it here.

-- Find unexpected zero link entries

SELECT oid, prsname
FROM pg_ts_parser
WHERE prsnamespace = 0 OR prsstart = 0 OR prstoken = 0 OR prsend = 0 OR
      -- prsheadline is optional
      prslextype = 0;

SELECT oid, dictname
FROM pg_ts_dict
WHERE dictnamespace = 0 OR dictowner = 0 OR dicttemplate = 0;

SELECT oid, tmplname
FROM pg_ts_template
WHERE tmplnamespace = 0 OR tmpllexize = 0;  -- tmplinit is optional

SELECT oid, cfgname
FROM pg_ts_config
WHERE cfgnamespace = 0 OR cfgowner = 0 OR cfgparser = 0;

SELECT mapcfg, maptokentype, mapseqno
FROM pg_ts_config_map
WHERE mapcfg = 0 OR mapdict = 0;

-- Look for pg_ts_config_map entries that aren't one of parser's token types
SELECT * FROM
  ( SELECT oid AS cfgid, (ts_token_type(cfgparser)).tokid AS tokid
    FROM pg_ts_config ) AS tt 
RIGHT JOIN pg_ts_config_map AS m
    ON (tt.cfgid=m.mapcfg AND tt.tokid=m.maptokentype)
WHERE
    tt.cfgid IS NULL OR tt.tokid IS NULL;

-- test basic text search behavior without indexes, then with

SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';

create index wowidx on test_tsvector using gist (a);

SET enable_seqscan=OFF;

SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';

RESET enable_seqscan;

drop index wowidx;

create index wowidx on test_tsvector using gin (a);

SET enable_seqscan=OFF;

SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';
  
RESET enable_seqscan;
insert into test_tsvector values ('???', 'DFG:1A,2B,6C,10 FGH');
select * from ts_stat('select a from test_tsvector') order by ndoc desc, nentry desc, word limit 10;
select * from ts_stat('select a from test_tsvector', 'AB') order by ndoc desc, nentry desc, word;

--dictionaries and to_tsvector

select ts_lexize('english', 'skies');
select ts_lexize('english', 'identity');

select * from ts_token_type('default');

select * from ts_parse('default', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234
<i <b> wow  < jqw <> qwerty');

select to_tsvector('english', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234
<i <b> wow  < jqw <> qwerty');

select length(to_tsvector('english', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
/usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234
<i <b> wow  < jqw <> qwerty'));

-- to_tsquery

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

select ts_rank_cd(to_tsvector('english', 'Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
'), to_tsquery('english', 'sea&thousand&years'));

select ts_rank_cd(to_tsvector('english', 'Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
'), to_tsquery('english', 'granite&sea'));

select ts_rank_cd(to_tsvector('english', 'Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
'), to_tsquery('english', 'sea'));

--headline tests
select ts_headline('english', 'Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
', to_tsquery('english', 'sea&thousand&years'));

select ts_headline('english', 'Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
', to_tsquery('english', 'granite&sea'));

select ts_headline('english', 'Erosion It took the sea a thousand years,
A thousand years to trace
The granite features of this cliff
In crag and scarp and base.
It took the sea an hour one night
An hour of storm to place
The sculpture of these granite seams,
Upon a woman s face. E.  J.  Pratt  (1882 1964)
', to_tsquery('english', 'sea'));

select ts_headline('english', '
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
to_tsquery('english', 'sea&foo'), 'HighlightAll=true');

--Rewrite sub system

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


select count(*) from test_tsquery where keyword <  'new & york';
select count(*) from test_tsquery where keyword <= 'new & york';
select count(*) from test_tsquery where keyword = 'new & york';
select count(*) from test_tsquery where keyword >= 'new & york';
select count(*) from test_tsquery where keyword >  'new & york';

create unique index bt_tsq on test_tsquery (keyword);

SET enable_seqscan=OFF;

select count(*) from test_tsquery where keyword <  'new & york';
select count(*) from test_tsquery where keyword <= 'new & york';
select count(*) from test_tsquery where keyword = 'new & york';
select count(*) from test_tsquery where keyword >= 'new & york';
select count(*) from test_tsquery where keyword >  'new & york';

RESET enable_seqscan;

select ts_rewrite('foo & bar & qq & new & york',  'new & york'::tsquery, 'big & apple | nyc | new & york & city');

select ts_rewrite('moscow', 'select keyword, sample from test_tsquery'::text );
select ts_rewrite('moscow & hotel', 'select keyword, sample from test_tsquery'::text );
select ts_rewrite('bar &  new & qq & foo & york', 'select keyword, sample from test_tsquery'::text );

select ts_rewrite( ARRAY['moscow', keyword, sample] ) from test_tsquery;
select ts_rewrite( ARRAY['moscow & hotel', keyword, sample] ) from test_tsquery;
select ts_rewrite( ARRAY['bar &  new & qq & foo & york', keyword, sample] ) from test_tsquery;


select keyword from test_tsquery where keyword @> 'new';
select keyword from test_tsquery where keyword @> 'moscow';
select keyword from test_tsquery where keyword <@ 'new';
select keyword from test_tsquery where keyword <@ 'moscow';
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow') as query where keyword <@ query;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow & hotel') as query where keyword <@ query;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'bar &  new & qq & foo & york') as query where keyword <@ query;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow') as query where query @> keyword;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow & hotel') as query where query @> keyword;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'bar &  new & qq & foo & york') as query where query @> keyword;

create index qq on test_tsquery using gist (keyword tsquery_ops);
SET enable_seqscan=OFF;

select keyword from test_tsquery where keyword @> 'new';
select keyword from test_tsquery where keyword @> 'moscow';
select keyword from test_tsquery where keyword <@ 'new';
select keyword from test_tsquery where keyword <@ 'moscow';
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow') as query where keyword <@ query;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow & hotel') as query where keyword <@ query;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'bar &  new & qq & foo & york') as query where keyword <@ query;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow') as query where query @> keyword;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'moscow & hotel') as query where query @> keyword;
select ts_rewrite( ARRAY[query, keyword, sample] ) from test_tsquery, to_tsquery('english', 'bar &  new & qq & foo & york') as query where query @> keyword;

RESET enable_seqscan;

--test GUC
set default_text_search_config=simple;

select to_tsvector('SKIES My booKs');
select plainto_tsquery('SKIES My booKs');
select to_tsquery('SKIES & My | booKs');

set default_text_search_config=english;

select to_tsvector('SKIES My booKs');
select plainto_tsquery('SKIES My booKs');
select to_tsquery('SKIES & My | booKs');

--trigger
CREATE TRIGGER tsvectorupdate
BEFORE UPDATE OR INSERT ON test_tsvector
FOR EACH ROW EXECUTE PROCEDURE tsvector_update_trigger(a, 'pg_catalog.english', t);

SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');
INSERT INTO test_tsvector (t) VALUES ('345 qwerty');
SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');
UPDATE test_tsvector SET t = null WHERE t = '345 qwerty';
SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');

insert into test_tsvector (t) values ('345 qwerty');

select count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');
