--
-- first, define the functions.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i tablefunc.sql
\set ECHO all

--
-- normal_rand()
-- no easy way to do this for regression testing
--
SELECT avg(normal_rand)::int FROM normal_rand(100, 250, 0.2, EXTRACT(SECONDS FROM CURRENT_TIME(0))::int);

--
-- crosstab()
--
create table ct(id int, rowclass text, rowid text, attribute text, value text);
\copy ct from 'data/ct.data'

select * from crosstab2('select rowid, attribute, value from ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') order by 1,2;');
select * from crosstab3('select rowid, attribute, value from ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') order by 1,2;');
select * from crosstab4('select rowid, attribute, value from ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') order by 1,2;');

select * from crosstab2('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;');
select * from crosstab3('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;');
select * from crosstab4('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;');

select * from crosstab2('select rowid, attribute, value from ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') order by 1,2;');
select * from crosstab3('select rowid, attribute, value from ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') order by 1,2;');
select * from crosstab4('select rowid, attribute, value from ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') order by 1,2;');

select * from crosstab2('select rowid, attribute, value from ct where rowclass = ''group2'' order by 1,2;');
select * from crosstab3('select rowid, attribute, value from ct where rowclass = ''group2'' order by 1,2;');
select * from crosstab4('select rowid, attribute, value from ct where rowclass = ''group2'' order by 1,2;');

select * from crosstab('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;', 2) as c(rowid text, att1 text, att2 text);
select * from crosstab('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;', 3) as c(rowid text, att1 text, att2 text, att3 text);
select * from crosstab('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;', 4) as c(rowid text, att1 text, att2 text, att3 text, att4 text);

-- test connectby with text based hierarchy
CREATE TABLE connectby_text(keyid text, parent_keyid text);
\copy connectby_text from 'data/connectby_text.data'

-- with branch
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'row2', 0, '~') AS t(keyid text, parent_keyid text, level int, branch text);

-- without branch
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'row2', 0) AS t(keyid text, parent_keyid text, level int);

-- test connectby with int based hierarchy
CREATE TABLE connectby_int(keyid int, parent_keyid int);
\copy connectby_int from 'data/connectby_int.data'

-- with branch
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid int, parent_keyid int, level int, branch text);

-- without branch
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0) AS t(keyid int, parent_keyid int, level int);

