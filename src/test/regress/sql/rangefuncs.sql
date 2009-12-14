SELECT name, setting FROM pg_settings WHERE name LIKE 'enable%';

CREATE TABLE foo2(fooid int, f2 int);
INSERT INTO foo2 VALUES(1, 11);
INSERT INTO foo2 VALUES(2, 22);
INSERT INTO foo2 VALUES(1, 111);

CREATE FUNCTION foot(int) returns setof foo2 as 'SELECT * FROM foo2 WHERE fooid = $1;' LANGUAGE SQL;

-- supposed to fail with ERROR
select * from foo2, foot(foo2.fooid) z where foo2.f2 = z.f2;

-- function in subselect
select * from foo2 where f2 in (select f2 from foot(foo2.fooid) z where z.fooid = foo2.fooid) ORDER BY 1,2;

-- function in subselect
select * from foo2 where f2 in (select f2 from foot(1) z where z.fooid = foo2.fooid) ORDER BY 1,2;

-- function in subselect
select * from foo2 where f2 in (select f2 from foot(foo2.fooid) z where z.fooid = 1) ORDER BY 1,2;

-- nested functions
select foot.fooid, foot.f2 from foot(sin(pi()/2)::int) ORDER BY 1,2;

CREATE TABLE foo (fooid int, foosubid int, fooname text, primary key(fooid,foosubid));
INSERT INTO foo VALUES(1,1,'Joe');
INSERT INTO foo VALUES(1,2,'Ed');
INSERT INTO foo VALUES(2,1,'Mary');

-- sql, proretset = f, prorettype = b
CREATE FUNCTION getfoo(int) RETURNS int AS 'SELECT $1;' LANGUAGE SQL;
SELECT * FROM getfoo(1) AS t1;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo(1);
SELECT * FROM vw_getfoo;

-- sql, proretset = t, prorettype = b
DROP VIEW vw_getfoo;
DROP FUNCTION getfoo(int);
CREATE FUNCTION getfoo(int) RETURNS setof int AS 'SELECT fooid FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo(1) AS t1;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo(1);
SELECT * FROM vw_getfoo;

-- sql, proretset = t, prorettype = b
DROP VIEW vw_getfoo;
DROP FUNCTION getfoo(int);
CREATE FUNCTION getfoo(int) RETURNS setof text AS 'SELECT fooname FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo(1) AS t1;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo(1);
SELECT * FROM vw_getfoo;

-- sql, proretset = f, prorettype = c
DROP VIEW vw_getfoo;
DROP FUNCTION getfoo(int);
CREATE FUNCTION getfoo(int) RETURNS foo AS 'SELECT * FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo(1) AS t1;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo(1);
SELECT * FROM vw_getfoo;

-- sql, proretset = t, prorettype = c
DROP VIEW vw_getfoo;
DROP FUNCTION getfoo(int);
CREATE FUNCTION getfoo(int) RETURNS setof foo AS 'SELECT * FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo(1) AS t1;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo(1);
SELECT * FROM vw_getfoo;

-- sql, proretset = f, prorettype = record
DROP VIEW vw_getfoo;
DROP FUNCTION getfoo(int);
CREATE FUNCTION getfoo(int) RETURNS RECORD AS 'SELECT * FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo(1) AS t1(fooid int, foosubid int, fooname text);
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo(1) AS 
(fooid int, foosubid int, fooname text);
SELECT * FROM vw_getfoo;

-- sql, proretset = t, prorettype = record
DROP VIEW vw_getfoo;
DROP FUNCTION getfoo(int);
CREATE FUNCTION getfoo(int) RETURNS setof record AS 'SELECT * FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo(1) AS t1(fooid int, foosubid int, fooname text);
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo(1) AS
(fooid int, foosubid int, fooname text);
SELECT * FROM vw_getfoo;

-- plpgsql, proretset = f, prorettype = b
DROP VIEW vw_getfoo;
DROP FUNCTION getfoo(int);
CREATE FUNCTION getfoo(int) RETURNS int AS 'DECLARE fooint int; BEGIN SELECT fooid into fooint FROM foo WHERE fooid = $1; RETURN fooint; END;' LANGUAGE plpgsql;
SELECT * FROM getfoo(1) AS t1;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo(1);
SELECT * FROM vw_getfoo;

-- plpgsql, proretset = f, prorettype = c
DROP VIEW vw_getfoo;
DROP FUNCTION getfoo(int);
CREATE FUNCTION getfoo(int) RETURNS foo AS 'DECLARE footup foo%ROWTYPE; BEGIN SELECT * into footup FROM foo WHERE fooid = $1; RETURN footup; END;' LANGUAGE plpgsql;
SELECT * FROM getfoo(1) AS t1;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo(1);
SELECT * FROM vw_getfoo;

DROP VIEW vw_getfoo;
DROP FUNCTION getfoo(int);
DROP FUNCTION foot(int);
DROP TABLE foo2;
DROP TABLE foo;

-- Rescan tests --
CREATE TABLE foorescan (fooid int, foosubid int, fooname text, primary key(fooid,foosubid));
INSERT INTO foorescan values(5000,1,'abc.5000.1');
INSERT INTO foorescan values(5001,1,'abc.5001.1');
INSERT INTO foorescan values(5002,1,'abc.5002.1');
INSERT INTO foorescan values(5003,1,'abc.5003.1');
INSERT INTO foorescan values(5004,1,'abc.5004.1');
INSERT INTO foorescan values(5005,1,'abc.5005.1');
INSERT INTO foorescan values(5006,1,'abc.5006.1');
INSERT INTO foorescan values(5007,1,'abc.5007.1');
INSERT INTO foorescan values(5008,1,'abc.5008.1');
INSERT INTO foorescan values(5009,1,'abc.5009.1');

INSERT INTO foorescan values(5000,2,'abc.5000.2');
INSERT INTO foorescan values(5001,2,'abc.5001.2');
INSERT INTO foorescan values(5002,2,'abc.5002.2');
INSERT INTO foorescan values(5003,2,'abc.5003.2');
INSERT INTO foorescan values(5004,2,'abc.5004.2');
INSERT INTO foorescan values(5005,2,'abc.5005.2');
INSERT INTO foorescan values(5006,2,'abc.5006.2');
INSERT INTO foorescan values(5007,2,'abc.5007.2');
INSERT INTO foorescan values(5008,2,'abc.5008.2');
INSERT INTO foorescan values(5009,2,'abc.5009.2');

INSERT INTO foorescan values(5000,3,'abc.5000.3');
INSERT INTO foorescan values(5001,3,'abc.5001.3');
INSERT INTO foorescan values(5002,3,'abc.5002.3');
INSERT INTO foorescan values(5003,3,'abc.5003.3');
INSERT INTO foorescan values(5004,3,'abc.5004.3');
INSERT INTO foorescan values(5005,3,'abc.5005.3');
INSERT INTO foorescan values(5006,3,'abc.5006.3');
INSERT INTO foorescan values(5007,3,'abc.5007.3');
INSERT INTO foorescan values(5008,3,'abc.5008.3');
INSERT INTO foorescan values(5009,3,'abc.5009.3');

INSERT INTO foorescan values(5000,4,'abc.5000.4');
INSERT INTO foorescan values(5001,4,'abc.5001.4');
INSERT INTO foorescan values(5002,4,'abc.5002.4');
INSERT INTO foorescan values(5003,4,'abc.5003.4');
INSERT INTO foorescan values(5004,4,'abc.5004.4');
INSERT INTO foorescan values(5005,4,'abc.5005.4');
INSERT INTO foorescan values(5006,4,'abc.5006.4');
INSERT INTO foorescan values(5007,4,'abc.5007.4');
INSERT INTO foorescan values(5008,4,'abc.5008.4');
INSERT INTO foorescan values(5009,4,'abc.5009.4');

INSERT INTO foorescan values(5000,5,'abc.5000.5');
INSERT INTO foorescan values(5001,5,'abc.5001.5');
INSERT INTO foorescan values(5002,5,'abc.5002.5');
INSERT INTO foorescan values(5003,5,'abc.5003.5');
INSERT INTO foorescan values(5004,5,'abc.5004.5');
INSERT INTO foorescan values(5005,5,'abc.5005.5');
INSERT INTO foorescan values(5006,5,'abc.5006.5');
INSERT INTO foorescan values(5007,5,'abc.5007.5');
INSERT INTO foorescan values(5008,5,'abc.5008.5');
INSERT INTO foorescan values(5009,5,'abc.5009.5');

CREATE FUNCTION foorescan(int,int) RETURNS setof foorescan AS 'SELECT * FROM foorescan WHERE fooid >= $1 and fooid < $2 ;' LANGUAGE SQL;

--invokes ExecFunctionReScan
SELECT * FROM foorescan f WHERE f.fooid IN (SELECT fooid FROM foorescan(5002,5004)) ORDER BY 1,2;

CREATE VIEW vw_foorescan AS SELECT * FROM foorescan(5002,5004);

--invokes ExecFunctionReScan
SELECT * FROM foorescan f WHERE f.fooid IN (SELECT fooid FROM vw_foorescan) ORDER BY 1,2;

CREATE TABLE barrescan (fooid int primary key);
INSERT INTO barrescan values(5003);
INSERT INTO barrescan values(5004);
INSERT INTO barrescan values(5005);
INSERT INTO barrescan values(5006);
INSERT INTO barrescan values(5007);
INSERT INTO barrescan values(5008);

CREATE FUNCTION foorescan(int) RETURNS setof foorescan AS 'SELECT * FROM foorescan WHERE fooid = $1;' LANGUAGE SQL;

--invokes ExecFunctionReScan with chgParam != NULL
SELECT f.* FROM barrescan b, foorescan f WHERE f.fooid = b.fooid AND b.fooid IN (SELECT fooid FROM foorescan(b.fooid)) ORDER BY 1,2;
SELECT b.fooid, max(f.foosubid) FROM barrescan b, foorescan f WHERE f.fooid = b.fooid AND b.fooid IN (SELECT fooid FROM foorescan(b.fooid)) GROUP BY b.fooid ORDER BY 1,2;

CREATE VIEW fooview1 AS SELECT f.* FROM barrescan b, foorescan f WHERE f.fooid = b.fooid AND b.fooid IN (SELECT fooid FROM foorescan(b.fooid)) ORDER BY 1,2;
SELECT * FROM fooview1 AS fv WHERE fv.fooid = 5004;

CREATE VIEW fooview2 AS SELECT b.fooid, max(f.foosubid) AS maxsubid FROM barrescan b, foorescan f WHERE f.fooid = b.fooid AND b.fooid IN (SELECT fooid FROM foorescan(b.fooid)) GROUP BY b.fooid ORDER BY 1,2;
SELECT * FROM fooview2 AS fv WHERE fv.maxsubid = 5;

DROP VIEW vw_foorescan;
DROP VIEW fooview1;
DROP VIEW fooview2;
DROP FUNCTION foorescan(int,int);
DROP FUNCTION foorescan(int);
DROP TABLE foorescan;
DROP TABLE barrescan;

--
-- Test cases involving OUT parameters
--

CREATE FUNCTION foo(in f1 int, out f2 int)
AS 'select $1+1' LANGUAGE sql;
SELECT foo(42);
SELECT * FROM foo(42);
SELECT * FROM foo(42) AS p(x);

-- explicit spec of return type is OK
CREATE OR REPLACE FUNCTION foo(in f1 int, out f2 int) RETURNS int
AS 'select $1+1' LANGUAGE sql;
-- error, wrong result type
CREATE OR REPLACE FUNCTION foo(in f1 int, out f2 int) RETURNS float
AS 'select $1+1' LANGUAGE sql;
-- with multiple OUT params you must get a RECORD result
CREATE OR REPLACE FUNCTION foo(in f1 int, out f2 int, out f3 text) RETURNS int
AS 'select $1+1' LANGUAGE sql;
CREATE OR REPLACE FUNCTION foo(in f1 int, out f2 int, out f3 text)
RETURNS record
AS 'select $1+1' LANGUAGE sql;

CREATE OR REPLACE FUNCTION foor(in f1 int, out f2 int, out text)
AS $$select $1-1, $1::text || 'z'$$ LANGUAGE sql;
SELECT f1, foor(f1) FROM int4_tbl;
SELECT * FROM foor(42);
SELECT * FROM foor(42) AS p(a,b);

CREATE OR REPLACE FUNCTION foob(in f1 int, inout f2 int, out text)
AS $$select $2-1, $1::text || 'z'$$ LANGUAGE sql;
SELECT f1, foob(f1, f1/2) FROM int4_tbl;
SELECT * FROM foob(42, 99);
SELECT * FROM foob(42, 99) AS p(a,b);

-- Can reference function with or without OUT params for DROP, etc
DROP FUNCTION foo(int);
DROP FUNCTION foor(in f2 int, out f1 int, out text);
DROP FUNCTION foob(in f1 int, inout f2 int);

--
-- For my next trick, polymorphic OUT parameters
--

CREATE FUNCTION dup (f1 anyelement, f2 out anyelement, f3 out anyarray)
AS 'select $1, array[$1,$1]' LANGUAGE sql;
SELECT dup(22);
SELECT dup('xyz');	-- fails
SELECT dup('xyz'::text);
SELECT * FROM dup('xyz'::text);

-- equivalent specification
CREATE OR REPLACE FUNCTION dup (inout f2 anyelement, out f3 anyarray)
AS 'select $1, array[$1,$1]' LANGUAGE sql;
SELECT dup(22);

DROP FUNCTION dup(anyelement);

-- fails, no way to deduce outputs
CREATE FUNCTION bad (f1 int, out f2 anyelement, out f3 anyarray)
AS 'select $1, array[$1,$1]' LANGUAGE sql;

--
-- table functions
--

CREATE OR REPLACE FUNCTION foo()
RETURNS TABLE(a int)
AS $$ SELECT a FROM generate_series(1,5) a(a) $$ LANGUAGE sql;
SELECT * FROM foo();
DROP FUNCTION foo();

CREATE OR REPLACE FUNCTION foo(int)
RETURNS TABLE(a int, b int)
AS $$ SELECT a, b
         FROM generate_series(1,$1) a(a),
              generate_series(1,$1) b(b) $$ LANGUAGE sql;
SELECT * FROM foo(3);
DROP FUNCTION foo(int);

--
-- some tests on SQL functions with RETURNING
--

create temp table tt(f1 serial, data text);

create function insert_tt(text) returns int as
$$ insert into tt(data) values($1) returning f1 $$
language sql;

select insert_tt('foo');
select insert_tt('bar');
select * from tt;

-- insert will execute to completion even if function needs just 1 row
create or replace function insert_tt(text) returns int as
$$ insert into tt(data) values($1),($1||$1) returning f1 $$
language sql;

select insert_tt('fool');
select * from tt;

-- setof does what's expected
create or replace function insert_tt2(text,text) returns setof int as
$$ insert into tt(data) values($1),($2) returning f1 $$
language sql;

select insert_tt2('foolish','barrish');
select * from insert_tt2('baz','quux');
select * from tt;

-- limit doesn't prevent execution to completion
select insert_tt2('foolish','barrish') limit 1;
select * from tt;

-- triggers will fire, too
create function noticetrigger() returns trigger as $$
begin
  raise notice 'noticetrigger % %', new.f1, new.data;
  return null;
end $$ language plpgsql;
create trigger tnoticetrigger after insert on tt for each row
execute procedure noticetrigger();

select insert_tt2('foolme','barme') limit 1;
select * from tt;

-- and rules work
create temp table tt_log(f1 int, data text);

create rule insert_tt_rule as on insert to tt do also
  insert into tt_log values(new.*);

select insert_tt2('foollog','barlog') limit 1;
select * from tt;
-- note that nextval() gets executed a second time in the rule expansion,
-- which is expected.
select * from tt_log;

-- test case for a whole-row-variable bug
create function foo1(n integer, out a text, out b text)
  returns setof record
  language sql
  as $$ select 'foo ' || i, 'bar ' || i from generate_series(1,$1) i $$;

set work_mem='64kB';
select t.a, t, t.a from foo1(10000) t limit 1;
reset work_mem;
select t.a, t, t.a from foo1(10000) t limit 1;

drop function foo1(n integer);

-- test use of SQL functions returning record
-- this is supported in some cases where the query doesn't specify
-- the actual record type ...

create function array_to_set(anyarray) returns setof record as $$
  select i AS "index", $1[i] AS "value" from generate_subscripts($1, 1) i
$$ language sql strict immutable;

select array_to_set(array['one', 'two']);
select * from array_to_set(array['one', 'two']) as t(f1 int,f2 text);
select * from array_to_set(array['one', 'two']); -- fail

create temp table foo(f1 int8, f2 int8);

create function testfoo() returns record as $$
  insert into foo values (1,2) returning *;
$$ language sql;

select testfoo();
select * from testfoo() as t(f1 int8,f2 int8);
select * from testfoo(); -- fail

drop function testfoo();

create function testfoo() returns setof record as $$
  insert into foo values (1,2), (3,4) returning *;
$$ language sql;

select testfoo();
select * from testfoo() as t(f1 int8,f2 int8);
select * from testfoo(); -- fail

drop function testfoo();

--
-- Check some cases involving dropped columns in a rowtype result
--

create temp table users (userid text, email text, todrop bool, enabled bool);
insert into users values ('id','email',true,true);
insert into users values ('id2','email2',true,true);
alter table users drop column todrop;

create or replace function get_first_user() returns users as
$$ SELECT * FROM users ORDER BY userid LIMIT 1; $$
language sql stable;

SELECT get_first_user();
SELECT * FROM get_first_user();

create or replace function get_users() returns setof users as
$$ SELECT * FROM users ORDER BY userid; $$
language sql stable;

SELECT get_users();
SELECT * FROM get_users();

drop function get_first_user();
drop function get_users();
drop table users;

-- this won't get inlined because of type coercion, but it shouldn't fail

create or replace function foobar() returns setof text as
$$ select 'foo'::varchar union all select 'bar'::varchar ; $$
language sql stable;

select foobar();
select * from foobar();

drop function foobar();
