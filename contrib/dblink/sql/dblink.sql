--
-- First, create a slave database and define the functions.
-- Turn off echoing so that expected file does not depend on
-- contents of dblink.sql.
--
CREATE DATABASE regression_slave;
\connect regression_slave
\set ECHO none
\i dblink.sql
\set ECHO all

create table foo(f1 int, f2 text, f3 text[], primary key (f1,f2));
insert into foo values(0,'a','{"a0","b0","c0"}');
insert into foo values(1,'b','{"a1","b1","c1"}');
insert into foo values(2,'c','{"a2","b2","c2"}');
insert into foo values(3,'d','{"a3","b3","c3"}');
insert into foo values(4,'e','{"a4","b4","c4"}');
insert into foo values(5,'f','{"a5","b5","c5"}');
insert into foo values(6,'g','{"a6","b6","c6"}');
insert into foo values(7,'h','{"a7","b7","c7"}');
insert into foo values(8,'i','{"a8","b8","c8"}');
insert into foo values(9,'j','{"a9","b9","c9"}');

-- misc utilities

-- show the currently executing query
select 'hello' as hello, dblink_current_query() as query;

-- list the primary key fields
select * from dblink_get_pkey('foo');

-- build an insert statement based on a local tuple,
-- replacing the primary key values with new ones
select dblink_build_sql_insert('foo','1 2',2,'{"0", "a"}','{"99", "xyz"}');

-- build an update statement based on a local tuple,
-- replacing the primary key values with new ones
select dblink_build_sql_update('foo','1 2',2,'{"0", "a"}','{"99", "xyz"}');

-- build a delete statement based on a local tuple,
select dblink_build_sql_delete('foo','1 2',2,'{"0", "a"}');

--
-- Connect back to the regression database and define the functions.
-- Turn off echoing so that expected file does not depend on
-- contents of dblink.sql.
--
\connect regression
\set ECHO none
\i dblink.sql
\set ECHO all

-- regular old dblink
select * from dblink('dbname=regression_slave','select * from foo') as t(a int, b text, c text[]) where t.a > 7;

-- should generate "no connection available" error
select * from dblink('select * from foo') as t(a int, b text, c text[]) where t.a > 7;

-- create a persistent connection
select dblink_connect('dbname=regression_slave');

-- use the persistent connection
select * from dblink('select * from foo') as t(a int, b text, c text[]) where t.a > 7;

-- open a cursor
select dblink_open('rmt_foo_cursor','select * from foo');

-- fetch some data
select * from dblink_fetch('rmt_foo_cursor',4) as t(a int, b text, c text[]);
select * from dblink_fetch('rmt_foo_cursor',4) as t(a int, b text, c text[]);

-- this one only finds two rows left
select * from dblink_fetch('rmt_foo_cursor',4) as t(a int, b text, c text[]);

-- close the cursor
select dblink_close('rmt_foo_cursor');

-- should generate "cursor rmt_foo_cursor does not exist" error
select * from dblink_fetch('rmt_foo_cursor',4) as t(a int, b text, c text[]);

-- close the persistent connection
select dblink_disconnect();

-- should generate "no connection available" error
select * from dblink('select * from foo') as t(a int, b text, c text[]) where t.a > 7;

-- put more data into our slave table, first using arbitrary connection syntax
-- but truncate the actual return value so we can use diff to check for success
select substr(dblink_exec('dbname=regression_slave','insert into foo values(10,''k'',''{"a10","b10","c10"}'')'),1,6);

-- create a persistent connection
select dblink_connect('dbname=regression_slave');

-- put more data into our slave table, using persistent connection syntax
-- but truncate the actual return value so we can use diff to check for success
select substr(dblink_exec('insert into foo values(11,''l'',''{"a11","b11","c11"}'')'),1,6);

-- let's see it
select * from dblink('select * from foo') as t(a int, b text, c text[]);

-- change some data
select dblink_exec('update foo set f3[2] = ''b99'' where f1 = 11');

-- let's see it
select * from dblink('select * from foo') as t(a int, b text, c text[]) where a = 11;

-- delete some data
select dblink_exec('delete from foo where f1 = 11');

-- let's see it
select * from dblink('select * from foo') as t(a int, b text, c text[]) where a = 11;

-- close the persistent connection
select dblink_disconnect();

-- now wait for the connection to the slave to be cleared before
-- we try to drop the database
CREATE FUNCTION wait() RETURNS TEXT AS '
DECLARE
	rec           record;
    cntr          int;
BEGIN
    cntr = 0;

    select into rec d.datname
    from pg_database d,
        (select pg_stat_get_backend_dbid(pg_stat_get_backend_idset()) AS dbid) b
    where d.oid = b.dbid and d.datname = ''regression_slave'';

    WHILE FOUND LOOP
        cntr = cntr + 1;

        select into rec d.datname
        from pg_database d,
            (select pg_stat_get_backend_dbid(pg_stat_get_backend_idset()) AS dbid) b
        where d.oid = b.dbid and d.datname = ''regression_slave'';

        -- safety valve
        if cntr > 1000 THEN
            EXIT;
        end if;
	END LOOP;
	RETURN ''OK'';
END;
' LANGUAGE 'plpgsql';
SELECT wait();

-- OK, safe to drop the slave
DROP DATABASE regression_slave;
