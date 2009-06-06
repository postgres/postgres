-- Adjust this setting to control where the objects get created.
SET search_path = public;

--
-- Define the functions and test data
-- therein.
--
-- Turn off echoing so that expected file does not depend on
-- contents of dblink.sql.
SET client_min_messages = warning;
\set ECHO none
\i dblink.sql
\set ECHO all
RESET client_min_messages;

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

-- retest using a quoted and schema qualified table
CREATE SCHEMA "MySchema";
CREATE TABLE "MySchema"."Foo"(f1 int, f2 text, f3 text[], primary key (f1,f2));
INSERT INTO "MySchema"."Foo" VALUES (0,'a','{"a0","b0","c0"}');

-- list the primary key fields
SELECT *
FROM dblink_get_pkey('"MySchema"."Foo"');

-- build an insert statement based on a local tuple,
-- replacing the primary key values with new ones
SELECT dblink_build_sql_insert('"MySchema"."Foo"','1 2',2,'{"0", "a"}','{"99", "xyz"}');

-- build an update statement based on a local tuple,
-- replacing the primary key values with new ones
SELECT dblink_build_sql_update('"MySchema"."Foo"','1 2',2,'{"0", "a"}','{"99", "xyz"}');

-- build a delete statement based on a local tuple,
SELECT dblink_build_sql_delete('"MySchema"."Foo"','1 2',2,'{"0", "a"}');

-- regular old dblink
SELECT *
FROM dblink('dbname=contrib_regression','SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- should generate "connection not available" error
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- create a persistent connection
SELECT dblink_connect('dbname=contrib_regression');

-- use the persistent connection
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- open a cursor with bad SQL and fail_on_error set to false
SELECT dblink_open('rmt_foo_cursor','SELECT * FROM foobar',false);

-- reset remote transaction state
SELECT dblink_exec('ABORT');

-- open a cursor
SELECT dblink_open('rmt_foo_cursor','SELECT * FROM foo');

-- close the cursor
SELECT dblink_close('rmt_foo_cursor',false);

-- open the cursor again
SELECT dblink_open('rmt_foo_cursor','SELECT * FROM foo');

-- fetch some data
SELECT *
FROM dblink_fetch('rmt_foo_cursor',4) AS t(a int, b text, c text[]);

SELECT *
FROM dblink_fetch('rmt_foo_cursor',4) AS t(a int, b text, c text[]);

-- this one only finds two rows left
SELECT *
FROM dblink_fetch('rmt_foo_cursor',4) AS t(a int, b text, c text[]);

-- intentionally botch a fetch
SELECT *
FROM dblink_fetch('rmt_foobar_cursor',4,false) AS t(a int, b text, c text[]);

-- reset remote transaction state
SELECT dblink_exec('ABORT');

-- close the wrong cursor
SELECT dblink_close('rmt_foobar_cursor',false);

-- should generate 'cursor "rmt_foo_cursor" not found' error
SELECT *
FROM dblink_fetch('rmt_foo_cursor',4) AS t(a int, b text, c text[]);

-- this time, 'cursor "rmt_foo_cursor" not found' as a notice
SELECT *
FROM dblink_fetch('rmt_foo_cursor',4,false) AS t(a int, b text, c text[]);

-- close the persistent connection
SELECT dblink_disconnect();

-- should generate "connection not available" error
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- put more data into our slave table, first using arbitrary connection syntax
-- but truncate the actual return value so we can use diff to check for success
SELECT substr(dblink_exec('dbname=contrib_regression','INSERT INTO foo VALUES(10,''k'',''{"a10","b10","c10"}'')'),1,6);

-- create a persistent connection
SELECT dblink_connect('dbname=contrib_regression');

-- put more data into our slave table, using persistent connection syntax
-- but truncate the actual return value so we can use diff to check for success
SELECT substr(dblink_exec('INSERT INTO foo VALUES(11,''l'',''{"a11","b11","c11"}'')'),1,6);

-- let's see it
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[]);

-- bad remote select
SELECT *
FROM dblink('SELECT * FROM foobar',false) AS t(a int, b text, c text[]);

-- change some data
SELECT dblink_exec('UPDATE foo SET f3[2] = ''b99'' WHERE f1 = 11');

-- let's see it
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE a = 11;

-- botch a change to some other data
SELECT dblink_exec('UPDATE foobar SET f3[2] = ''b99'' WHERE f1 = 11',false);

-- delete some data
SELECT dblink_exec('DELETE FROM foo WHERE f1 = 11');

-- let's see it
SELECT *
FROM dblink('SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE a = 11;

-- close the persistent connection
SELECT dblink_disconnect();

--
-- tests for the new named persistent connection syntax
--

-- should generate "missing "=" after "myconn" in connection info string" error
SELECT *
FROM dblink('myconn','SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- create a named persistent connection
SELECT dblink_connect('myconn','dbname=contrib_regression');

-- use the named persistent connection
SELECT *
FROM dblink('myconn','SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- use the named persistent connection, but get it wrong
SELECT *
FROM dblink('myconn','SELECT * FROM foobar',false) AS t(a int, b text, c text[])
WHERE t.a > 7;

-- create a second named persistent connection
-- should error with "duplicate connection name"
SELECT dblink_connect('myconn','dbname=contrib_regression');

-- create a second named persistent connection with a new name
SELECT dblink_connect('myconn2','dbname=contrib_regression');

-- use the second named persistent connection
SELECT *
FROM dblink('myconn2','SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- close the second named persistent connection
SELECT dblink_disconnect('myconn2');

-- open a cursor incorrectly
SELECT dblink_open('myconn','rmt_foo_cursor','SELECT * FROM foobar',false);

-- reset remote transaction state
SELECT dblink_exec('myconn','ABORT');

-- test opening cursor in a transaction
SELECT dblink_exec('myconn','BEGIN');

-- an open transaction will prevent dblink_open() from opening its own
SELECT dblink_open('myconn','rmt_foo_cursor','SELECT * FROM foo');

-- this should not commit the transaction because the client opened it
SELECT dblink_close('myconn','rmt_foo_cursor');

-- this should succeed because we have an open transaction
SELECT dblink_exec('myconn','DECLARE xact_test CURSOR FOR SELECT * FROM foo');

-- commit remote transaction
SELECT dblink_exec('myconn','COMMIT');

-- test automatic transactions for multiple cursor opens
SELECT dblink_open('myconn','rmt_foo_cursor','SELECT * FROM foo');

-- the second cursor
SELECT dblink_open('myconn','rmt_foo_cursor2','SELECT * FROM foo');

-- this should not commit the transaction
SELECT dblink_close('myconn','rmt_foo_cursor2');

-- this should succeed because we have an open transaction
SELECT dblink_exec('myconn','DECLARE xact_test CURSOR FOR SELECT * FROM foo');

-- this should commit the transaction
SELECT dblink_close('myconn','rmt_foo_cursor');

-- this should fail because there is no open transaction
SELECT dblink_exec('myconn','DECLARE xact_test CURSOR FOR SELECT * FROM foo');

-- reset remote transaction state
SELECT dblink_exec('myconn','ABORT');

-- open a cursor
SELECT dblink_open('myconn','rmt_foo_cursor','SELECT * FROM foo');

-- fetch some data
SELECT *
FROM dblink_fetch('myconn','rmt_foo_cursor',4) AS t(a int, b text, c text[]);

SELECT *
FROM dblink_fetch('myconn','rmt_foo_cursor',4) AS t(a int, b text, c text[]);

-- this one only finds three rows left
SELECT *
FROM dblink_fetch('myconn','rmt_foo_cursor',4) AS t(a int, b text, c text[]);

-- fetch some data incorrectly
SELECT *
FROM dblink_fetch('myconn','rmt_foobar_cursor',4,false) AS t(a int, b text, c text[]);

-- reset remote transaction state
SELECT dblink_exec('myconn','ABORT');

-- should generate 'cursor "rmt_foo_cursor" not found' error
SELECT *
FROM dblink_fetch('myconn','rmt_foo_cursor',4) AS t(a int, b text, c text[]);

-- close the named persistent connection
SELECT dblink_disconnect('myconn');

-- should generate "missing "=" after "myconn" in connection info string" error
SELECT *
FROM dblink('myconn','SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE t.a > 7;

-- create a named persistent connection
SELECT dblink_connect('myconn','dbname=contrib_regression');

-- put more data into our slave table, using named persistent connection syntax
-- but truncate the actual return value so we can use diff to check for success
SELECT substr(dblink_exec('myconn','INSERT INTO foo VALUES(11,''l'',''{"a11","b11","c11"}'')'),1,6);

-- let's see it
SELECT *
FROM dblink('myconn','SELECT * FROM foo') AS t(a int, b text, c text[]);

-- change some data
SELECT dblink_exec('myconn','UPDATE foo SET f3[2] = ''b99'' WHERE f1 = 11');

-- let's see it
SELECT *
FROM dblink('myconn','SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE a = 11;

-- delete some data
SELECT dblink_exec('myconn','DELETE FROM foo WHERE f1 = 11');

-- let's see it
SELECT *
FROM dblink('myconn','SELECT * FROM foo') AS t(a int, b text, c text[])
WHERE a = 11;

-- close the named persistent connection
SELECT dblink_disconnect('myconn');

-- close the named persistent connection again
-- should get 'connection "myconn" not available' error
SELECT dblink_disconnect('myconn');

-- test asynchronous queries
SELECT dblink_connect('dtest1', 'dbname=contrib_regression');
SELECT * from 
 dblink_send_query('dtest1', 'select * from foo where f1 < 3') as t1;

SELECT dblink_connect('dtest2', 'dbname=contrib_regression');
SELECT * from 
 dblink_send_query('dtest2', 'select * from foo where f1 > 2 and f1 < 7') as t1;

SELECT dblink_connect('dtest3', 'dbname=contrib_regression');
SELECT * from 
 dblink_send_query('dtest3', 'select * from foo where f1 > 6') as t1;

CREATE TEMPORARY TABLE result AS
(SELECT * from dblink_get_result('dtest1') as t1(f1 int, f2 text, f3 text[]))
UNION
(SELECT * from dblink_get_result('dtest2') as t2(f1 int, f2 text, f3 text[]))
UNION
(SELECT * from dblink_get_result('dtest3') as t3(f1 int, f2 text, f3 text[]))
ORDER by f1;

-- dblink_get_connections returns an array with elements in a machine-dependent
-- ordering, so we must resort to unnesting and sorting for a stable result
create function unnest(anyarray) returns setof anyelement
language sql strict immutable as $$
select $1[i] from generate_series(array_lower($1,1), array_upper($1,1)) as i
$$;

SELECT * FROM unnest(dblink_get_connections()) ORDER BY 1;

SELECT dblink_is_busy('dtest1');

SELECT dblink_disconnect('dtest1');
SELECT dblink_disconnect('dtest2');
SELECT dblink_disconnect('dtest3');

SELECT * from result;

SELECT dblink_connect('dtest1', 'dbname=contrib_regression');
SELECT * from 
 dblink_send_query('dtest1', 'select * from foo where f1 < 3') as t1;

SELECT dblink_cancel_query('dtest1');
SELECT dblink_error_message('dtest1');
SELECT dblink_disconnect('dtest1');

-- test foreign data wrapper functionality
CREATE USER dblink_regression_test;

CREATE FOREIGN DATA WRAPPER postgresql;
CREATE SERVER fdtest FOREIGN DATA WRAPPER postgresql OPTIONS (dbname 'contrib_regression');
CREATE USER MAPPING FOR public SERVER fdtest;
GRANT USAGE ON FOREIGN SERVER fdtest TO dblink_regression_test;
GRANT EXECUTE ON FUNCTION dblink_connect_u(text, text) TO dblink_regression_test;

\set ORIGINAL_USER :USER
\c - dblink_regression_test
-- should fail
SELECT dblink_connect('myconn', 'fdtest');
-- should succeed
SELECT dblink_connect_u('myconn', 'fdtest');
SELECT * FROM dblink('myconn','SELECT * FROM foo') AS t(a int, b text, c text[]);

\c - :ORIGINAL_USER
REVOKE USAGE ON FOREIGN SERVER fdtest FROM dblink_regression_test;
REVOKE EXECUTE ON FUNCTION dblink_connect_u(text, text) FROM dblink_regression_test;
DROP USER dblink_regression_test;
DROP USER MAPPING FOR public SERVER fdtest;
DROP SERVER fdtest;
DROP FOREIGN DATA WRAPPER postgresql;
