--
-- First, create a slave database and define the functions and test data
-- therein.
--
-- This initial hackery is to allow successive runs without failures.
--

-- Adjust this setting to control where the objects get created.
SET search_path = public;

CREATE OR REPLACE FUNCTION conditional_drop()
RETURNS text AS '
DECLARE
    dbname    text;
BEGIN
    SELECT INTO dbname datname FROM pg_database WHERE datname = ''regression_slave'';
    IF FOUND THEN
        DROP DATABASE regression_slave;
    END IF;
    RETURN ''OK'';
END;
' LANGUAGE 'plpgsql';
SELECT conditional_drop();

CREATE DATABASE regression_slave;
\connect regression_slave

-- Turn off echoing so that expected file does not depend on
-- contents of dblink.sql.
\set ECHO none
SET autocommit TO 'on';
\i dblink.sql
\set ECHO all

CREATE TABLE foo(f1 int, f2 text, f3 text[], primary key (f1,f2));
INSERT INTO foo VALUES (0,'a','{"a0","b0","c0"}');
INSERT INTO foo VALUES (1,'b','{"a1","b1","c1"}');
INSERT INTO foo VALUES (2,'c','{"a2","b2","c2"}');
INSERT INTO foo VALUES (3,'d','{"a3","b3","c3"}');
INSERT INTO foo VALUES (4,'e','{"a4","b4","c4"}');
INSERT INTO foo VALUES (5,'f','{"a5","b5","c5"}');
INSERT INTO foo VALUES (6,'g','{"a6","b6","c6"}');
INSERT INTO foo VALUES (7,'h','{"a7","b7","c7"}');
INSERT INTO foo VALUES (8,'i','{"a8","b8","c8"}');
INSERT INTO foo VALUES (9,'j','{"a9","b9","c9"}');

-- misc utilities

-- show the currently executing query
SELECT 'hello' AS hello, dblink_current_query() AS query;

-- list the primary key fields
SELECT *
FROM dblink_get_pkey('foo');

-- build an insert statement based on a local tuple,
-- replacing the primary key values with new ones
SELECT dblink_build_sql_insert('foo','1 2',2,'{"0", "a"}','{"99", "xyz"}');

-- build an update statement based on a local tuple,
-- replacing the primary key values with new ones
SELECT dblink_build_sql_update('foo','1 2',2,'{"0", "a"}','{"99", "xyz"}');

-- build a delete statement based on a local tuple,
SELECT dblink_build_sql_delete('foo','1 2',2,'{"0", "a"}');

--
-- Connect back to the regression database and define the functions.
-- Turn off echoing so that expected file does not depend on
-- contents of dblink.sql.
--
\connect regression
\set ECHO none
SET autocommit TO 'on';
\i dblink.sql
\set ECHO all

-- regular old dblink
SELECT *
FROM dblink('dbname=regression_slave','SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- should generate "no connection available" error
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- create a persistent connection
SELECT dblink_connect('dbname=regression_slave');

-- use the persistent connection
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- open a cursor
SELECT dblink_open('rmt_foo_cursor','SELECT * FROM foo');

-- fetch some data
SELECT *
FROM dblink_fetch('rmt_foo_cursor',4) AS t(a int, b text, c text[]);

SELECT *
FROM dblink_fetch('rmt_foo_cursor',4) AS t(a int, b text, c text[]);

-- this one only finds two rows left
SELECT *
FROM dblink_fetch('rmt_foo_cursor',4) AS t(a int, b text, c text[]);

-- close the cursor
SELECT dblink_close('rmt_foo_cursor');

-- should generate "cursor rmt_foo_cursor does not exist" error
SELECT *
FROM dblink_fetch('rmt_foo_cursor',4) AS t(a int, b text, c text[]);

-- close the persistent connection
SELECT dblink_disconnect();

-- should generate "no connection available" error
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- put more data into our slave table, first using arbitrary connection syntax
-- but truncate the actual return value so we can use diff to check for success
SELECT substr(dblink_exec('dbname=regression_slave','SET autocommit TO ''on'';INSERT INTO foo VALUES(10,''k'',''{"a10","b10","c10"}'')'),1,6);

-- create a persistent connection
SELECT dblink_connect('dbname=regression_slave');

-- put more data into our slave table, using persistent connection syntax
-- but truncate the actual return value so we can use diff to check for success
SELECT substr(dblink_exec('INSERT INTO foo VALUES(11,''l'',''{"a11","b11","c11"}'')'),1,6);

-- let's see it
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[]);

-- change some data
SELECT dblink_exec('UPDATE foo SET f3[2] = ''b99'' WHERE f1 = 11');

-- let's see it
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE a = 11;

-- delete some data
SELECT dblink_exec('DELETE FROM foo WHERE f1 = 11');

-- let's see it
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE a = 11;

-- close the persistent connection
SELECT dblink_disconnect();
