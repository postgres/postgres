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
SELECT avg(normal_rand)::int FROM normal_rand(100, 250, 0.2);

--
-- crosstab()
--
CREATE TABLE ct(id int, rowclass text, rowid text, attribute text, val text);
\copy ct from 'data/ct.data'

SELECT * FROM crosstab2('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;');
SELECT * FROM crosstab3('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;');
SELECT * FROM crosstab4('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') ORDER BY 1,2;');

SELECT * FROM crosstab2('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;');
SELECT * FROM crosstab3('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;');
SELECT * FROM crosstab4('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;');

SELECT * FROM crosstab2('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') ORDER BY 1,2;');
SELECT * FROM crosstab3('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') ORDER BY 1,2;');
SELECT * FROM crosstab4('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') ORDER BY 1,2;');

SELECT * FROM crosstab2('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' ORDER BY 1,2;');
SELECT * FROM crosstab3('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' ORDER BY 1,2;');
SELECT * FROM crosstab4('SELECT rowid, attribute, val FROM ct where rowclass = ''group2'' ORDER BY 1,2;');

SELECT * FROM crosstab('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;', 2) AS c(rowid text, att1 text, att2 text);
SELECT * FROM crosstab('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;', 3) AS c(rowid text, att1 text, att2 text, att3 text);
SELECT * FROM crosstab('SELECT rowid, attribute, val FROM ct where rowclass = ''group1'' ORDER BY 1,2;', 4) AS c(rowid text, att1 text, att2 text, att3 text, att4 text);

--
-- hash based crosstab
--
create table cth(id serial, rowid text, rowdt timestamp, attribute text, val text);
insert into cth values(DEFAULT,'test1','01 March 2003','temperature','42');
insert into cth values(DEFAULT,'test1','01 March 2003','test_result','PASS');
-- the next line is intentionally left commented and is therefore a "missing" attribute
-- insert into cth values(DEFAULT,'test1','01 March 2003','test_startdate','28 February 2003');
insert into cth values(DEFAULT,'test1','01 March 2003','volts','2.6987');
insert into cth values(DEFAULT,'test2','02 March 2003','temperature','53');
insert into cth values(DEFAULT,'test2','02 March 2003','test_result','FAIL');
insert into cth values(DEFAULT,'test2','02 March 2003','test_startdate','01 March 2003');
insert into cth values(DEFAULT,'test2','02 March 2003','volts','3.1234');

-- return attributes as plain text
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature text, test_result text, test_startdate text, volts text);

-- this time without rowdt
SELECT * FROM crosstab(
  'SELECT rowid, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, temperature text, test_result text, test_startdate text, volts text);

-- convert attributes to specific datatypes
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp, volts float8);

-- source query and category query out of sync
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth WHERE attribute IN (''temperature'',''test_result'',''test_startdate'') ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp);

-- if category query generates no rows, get expected error
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth WHERE attribute = ''a'' ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp, volts float8);

-- if category query generates more than one column, get expected error
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth ORDER BY 1',
  'SELECT DISTINCT rowdt, attribute FROM cth ORDER BY 2')
AS c(rowid text, rowdt timestamp, temperature int4, test_result text, test_startdate timestamp, volts float8);

-- if source query returns zero rows, get zero rows returned
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth WHERE false ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature text, test_result text, test_startdate text, volts text);

-- if source query returns zero rows, get zero rows returned even if category query generates no rows
SELECT * FROM crosstab(
  'SELECT rowid, rowdt, attribute, val FROM cth WHERE false ORDER BY 1',
  'SELECT DISTINCT attribute FROM cth WHERE false ORDER BY 1')
AS c(rowid text, rowdt timestamp, temperature text, test_result text, test_startdate text, volts text);

--
-- connectby
--

-- test connectby with text based hierarchy
CREATE TABLE connectby_text(keyid text, parent_keyid text, pos int);
\copy connectby_text from 'data/connectby_text.data'

-- with branch, without orderby
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'row2', 0, '~') AS t(keyid text, parent_keyid text, level int, branch text);

-- without branch, without orderby
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'row2', 0) AS t(keyid text, parent_keyid text, level int);

-- with branch, with orderby
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'pos', 'row2', 0, '~') AS t(keyid text, parent_keyid text, level int, branch text, pos int) ORDER BY t.pos;

-- without branch, with orderby
SELECT * FROM connectby('connectby_text', 'keyid', 'parent_keyid', 'pos', 'row2', 0) AS t(keyid text, parent_keyid text, level int, pos int) ORDER BY t.pos;

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

