--
-- first, define the functions.  Turn off echoing so that expected file
-- does not depend on contents of tablefunc.sql.
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

-- recursion detection
INSERT INTO connectby_int VALUES(10,9);
INSERT INTO connectby_int VALUES(11,10);
INSERT INTO connectby_int VALUES(9,11);

-- should fail due to infinite recursion
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 0, '~') AS t(keyid int, parent_keyid int, level int, branch text);

-- infinite recursion failure avoided by depth limit
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '2', 4, '~') AS t(keyid int, parent_keyid int, level int, branch text);

-- test for falsely detected recursion
DROP TABLE connectby_int;
CREATE TABLE connectby_int(keyid int, parent_keyid int);
INSERT INTO connectby_int VALUES(11,NULL);
INSERT INTO connectby_int VALUES(10,11);
INSERT INTO connectby_int VALUES(111,11);
INSERT INTO connectby_int VALUES(1,111);
-- this should not fail due to recursion detection
SELECT * FROM connectby('connectby_int', 'keyid', 'parent_keyid', '11', 0, '-') AS t(keyid int, parent_keyid int, level int, branch text);

