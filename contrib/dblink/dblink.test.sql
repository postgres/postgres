\connect dblink_test_slave

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

\connect dblink_test_master

-- regular old dblink
select * from dblink('dbname=dblink_test_slave','select * from foo') as t(a int, b text, c text[]) where t.a > 7;

-- should generate "no connection available" error
select * from dblink('select * from foo') as t(a int, b text, c text[]) where t.a > 7;

-- create a persistent connection
select dblink_connect('dbname=dblink_test_slave');

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
select substr(dblink_exec('dbname=dblink_test_slave','insert into foo values(10,''k'',''{"a10","b10","c10"}'')'),1,6);

-- create a persistent connection
select dblink_connect('dbname=dblink_test_slave');

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

-- misc utilities
\connect dblink_test_slave

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
