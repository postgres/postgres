SELECT name, setting FROM pg_settings WHERE name LIKE 'enable%';

CREATE TABLE foo2(fooid int, f2 int);
INSERT INTO foo2 VALUES(1, 11);
INSERT INTO foo2 VALUES(2, 22);
INSERT INTO foo2 VALUES(1, 111);

CREATE FUNCTION foot(int) returns setof foo2 as 'SELECT * FROM foo2 WHERE fooid = $1 ORDER BY f2;' LANGUAGE SQL;

-- function with ORDINALITY
select * from foot(1) with ordinality as z(a,b,ord);
select * from foot(1) with ordinality as z(a,b,ord) where b > 100;   -- ordinal 2, not 1
-- ordinality vs. column names and types
select a,b,ord from foot(1) with ordinality as z(a,b,ord);
select a,ord from unnest(array['a','b']) with ordinality as z(a,ord);
select * from unnest(array['a','b']) with ordinality as z(a,ord);
select a,ord from unnest(array[1.0::float8]) with ordinality as z(a,ord);
select * from unnest(array[1.0::float8]) with ordinality as z(a,ord);
select row_to_json(s.*) from generate_series(11,14) with ordinality s;
-- ordinality vs. views
create temporary view vw_ord as select * from (values (1)) v(n) join foot(1) with ordinality as z(a,b,ord) on (n=ord);
select * from vw_ord;
select definition from pg_views where viewname='vw_ord';
drop view vw_ord;

-- multiple functions
select * from rows from(foot(1),foot(2)) with ordinality as z(a,b,c,d,ord);
create temporary view vw_ord as select * from (values (1)) v(n) join rows from(foot(1),foot(2)) with ordinality as z(a,b,c,d,ord) on (n=ord);
select * from vw_ord;
select definition from pg_views where viewname='vw_ord';
drop view vw_ord;

-- expansions of unnest()
select * from unnest(array[10,20],array['foo','bar'],array[1.0]);
select * from unnest(array[10,20],array['foo','bar'],array[1.0]) with ordinality as z(a,b,c,ord);
select * from rows from(unnest(array[10,20],array['foo','bar'],array[1.0])) with ordinality as z(a,b,c,ord);
select * from rows from(unnest(array[10,20],array['foo','bar']), generate_series(101,102)) with ordinality as z(a,b,c,ord);
create temporary view vw_ord as select * from unnest(array[10,20],array['foo','bar'],array[1.0]) as z(a,b,c);
select * from vw_ord;
select definition from pg_views where viewname='vw_ord';
drop view vw_ord;
create temporary view vw_ord as select * from rows from(unnest(array[10,20],array['foo','bar'],array[1.0])) as z(a,b,c);
select * from vw_ord;
select definition from pg_views where viewname='vw_ord';
drop view vw_ord;
create temporary view vw_ord as select * from rows from(unnest(array[10,20],array['foo','bar']), generate_series(1,2)) as z(a,b,c);
select * from vw_ord;
select definition from pg_views where viewname='vw_ord';
drop view vw_ord;

-- ordinality and multiple functions vs. rewind and reverse scan
begin;
declare foo scroll cursor for select * from rows from(generate_series(1,5),generate_series(1,2)) with ordinality as g(i,j,o);
fetch all from foo;
fetch backward all from foo;
fetch all from foo;
fetch next from foo;
fetch next from foo;
fetch prior from foo;
fetch absolute 1 from foo;
fetch next from foo;
fetch next from foo;
fetch next from foo;
fetch prior from foo;
fetch prior from foo;
fetch prior from foo;
commit;

-- function with implicit LATERAL
select * from foo2, foot(foo2.fooid) z where foo2.f2 = z.f2;

-- function with implicit LATERAL and explicit ORDINALITY
select * from foo2, foot(foo2.fooid) with ordinality as z(fooid,f2,ord) where foo2.f2 = z.f2;

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
CREATE FUNCTION getfoo1(int) RETURNS int AS 'SELECT $1;' LANGUAGE SQL;
SELECT * FROM getfoo1(1) AS t1;
SELECT * FROM getfoo1(1) WITH ORDINALITY AS t1(v,o);
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo1(1);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo1(1) WITH ORDINALITY as t1(v,o);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;

-- sql, proretset = t, prorettype = b
CREATE FUNCTION getfoo2(int) RETURNS setof int AS 'SELECT fooid FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo2(1) AS t1;
SELECT * FROM getfoo2(1) WITH ORDINALITY AS t1(v,o);
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo2(1);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo2(1) WITH ORDINALITY AS t1(v,o);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;

-- sql, proretset = t, prorettype = b
CREATE FUNCTION getfoo3(int) RETURNS setof text AS 'SELECT fooname FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo3(1) AS t1;
SELECT * FROM getfoo3(1) WITH ORDINALITY AS t1(v,o);
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo3(1);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo3(1) WITH ORDINALITY AS t1(v,o);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;

-- sql, proretset = f, prorettype = c
CREATE FUNCTION getfoo4(int) RETURNS foo AS 'SELECT * FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo4(1) AS t1;
SELECT * FROM getfoo4(1) WITH ORDINALITY AS t1(a,b,c,o);
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo4(1);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo4(1) WITH ORDINALITY AS t1(a,b,c,o);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;

-- sql, proretset = t, prorettype = c
CREATE FUNCTION getfoo5(int) RETURNS setof foo AS 'SELECT * FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo5(1) AS t1;
SELECT * FROM getfoo5(1) WITH ORDINALITY AS t1(a,b,c,o);
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo5(1);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo5(1) WITH ORDINALITY AS t1(a,b,c,o);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;

-- sql, proretset = f, prorettype = record
CREATE FUNCTION getfoo6(int) RETURNS RECORD AS 'SELECT * FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo6(1) AS t1(fooid int, foosubid int, fooname text);
SELECT * FROM ROWS FROM( getfoo6(1) AS (fooid int, foosubid int, fooname text) ) WITH ORDINALITY;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo6(1) AS
(fooid int, foosubid int, fooname text);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;
CREATE VIEW vw_getfoo AS
  SELECT * FROM ROWS FROM( getfoo6(1) AS (fooid int, foosubid int, fooname text) )
                WITH ORDINALITY;
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;

-- sql, proretset = t, prorettype = record
CREATE FUNCTION getfoo7(int) RETURNS setof record AS 'SELECT * FROM foo WHERE fooid = $1;' LANGUAGE SQL;
SELECT * FROM getfoo7(1) AS t1(fooid int, foosubid int, fooname text);
SELECT * FROM ROWS FROM( getfoo7(1) AS (fooid int, foosubid int, fooname text) ) WITH ORDINALITY;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo7(1) AS
(fooid int, foosubid int, fooname text);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;
CREATE VIEW vw_getfoo AS
  SELECT * FROM ROWS FROM( getfoo7(1) AS (fooid int, foosubid int, fooname text) )
                WITH ORDINALITY;
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;

-- plpgsql, proretset = f, prorettype = b
CREATE FUNCTION getfoo8(int) RETURNS int AS 'DECLARE fooint int; BEGIN SELECT fooid into fooint FROM foo WHERE fooid = $1; RETURN fooint; END;' LANGUAGE plpgsql;
SELECT * FROM getfoo8(1) AS t1;
SELECT * FROM getfoo8(1) WITH ORDINALITY AS t1(v,o);
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo8(1);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo8(1) WITH ORDINALITY AS t1(v,o);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;

-- plpgsql, proretset = f, prorettype = c
CREATE FUNCTION getfoo9(int) RETURNS foo AS 'DECLARE footup foo%ROWTYPE; BEGIN SELECT * into footup FROM foo WHERE fooid = $1; RETURN footup; END;' LANGUAGE plpgsql;
SELECT * FROM getfoo9(1) AS t1;
SELECT * FROM getfoo9(1) WITH ORDINALITY AS t1(a,b,c,o);
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo9(1);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;
CREATE VIEW vw_getfoo AS SELECT * FROM getfoo9(1) WITH ORDINALITY AS t1(a,b,c,o);
SELECT * FROM vw_getfoo;
DROP VIEW vw_getfoo;

-- mix 'n match kinds, to exercise expandRTE and related logic

select * from rows from(getfoo1(1),getfoo2(1),getfoo3(1),getfoo4(1),getfoo5(1),
                    getfoo6(1) AS (fooid int, foosubid int, fooname text),
                    getfoo7(1) AS (fooid int, foosubid int, fooname text),
                    getfoo8(1),getfoo9(1))
              with ordinality as t1(a,b,c,d,e,f,g,h,i,j,k,l,m,o,p,q,r,s,t,u);
select * from rows from(getfoo9(1),getfoo8(1),
                    getfoo7(1) AS (fooid int, foosubid int, fooname text),
                    getfoo6(1) AS (fooid int, foosubid int, fooname text),
                    getfoo5(1),getfoo4(1),getfoo3(1),getfoo2(1),getfoo1(1))
              with ordinality as t1(a,b,c,d,e,f,g,h,i,j,k,l,m,o,p,q,r,s,t,u);

create temporary view vw_foo as
  select * from rows from(getfoo9(1),
                      getfoo7(1) AS (fooid int, foosubid int, fooname text),
                      getfoo1(1))
                with ordinality as t1(a,b,c,d,e,f,g,n);
select * from vw_foo;
select pg_get_viewdef('vw_foo');
drop view vw_foo;

DROP FUNCTION getfoo1(int);
DROP FUNCTION getfoo2(int);
DROP FUNCTION getfoo3(int);
DROP FUNCTION getfoo4(int);
DROP FUNCTION getfoo5(int);
DROP FUNCTION getfoo6(int);
DROP FUNCTION getfoo7(int);
DROP FUNCTION getfoo8(int);
DROP FUNCTION getfoo9(int);
DROP FUNCTION foot(int);
DROP TABLE foo2;
DROP TABLE foo;

-- Rescan tests --
CREATE TEMPORARY SEQUENCE foo_rescan_seq1;
CREATE TEMPORARY SEQUENCE foo_rescan_seq2;
CREATE TYPE foo_rescan_t AS (i integer, s bigint);

CREATE FUNCTION foo_sql(int,int) RETURNS setof foo_rescan_t AS 'SELECT i, nextval(''foo_rescan_seq1'') FROM generate_series($1,$2) i;' LANGUAGE SQL;
-- plpgsql functions use materialize mode
CREATE FUNCTION foo_mat(int,int) RETURNS setof foo_rescan_t AS 'begin for i in $1..$2 loop return next (i, nextval(''foo_rescan_seq2'')); end loop; end;' LANGUAGE plpgsql;

--invokes ExecReScanFunctionScan - all these cases should materialize the function only once
-- LEFT JOIN on a condition that the planner can't prove to be true is used to ensure the function
-- is on the inner path of a nestloop join

SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN foo_sql(11,13) ON (r+i)<100;
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN foo_sql(11,13) WITH ORDINALITY AS f(i,s,o) ON (r+i)<100;

SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN foo_mat(11,13) ON (r+i)<100;
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN foo_mat(11,13) WITH ORDINALITY AS f(i,s,o) ON (r+i)<100;
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN ROWS FROM( foo_sql(11,13), foo_mat(11,13) ) WITH ORDINALITY AS f(i1,s1,i2,s2,o) ON (r+i1+i2)<100;

SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN generate_series(11,13) f(i) ON (r+i)<100;
SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN generate_series(11,13) WITH ORDINALITY AS f(i,o) ON (r+i)<100;

SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN unnest(array[10,20,30]) f(i) ON (r+i)<100;
SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN unnest(array[10,20,30]) WITH ORDINALITY AS f(i,o) ON (r+i)<100;

--invokes ExecReScanFunctionScan with chgParam != NULL (using implied LATERAL)

SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), foo_sql(10+r,13);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), foo_sql(10+r,13) WITH ORDINALITY AS f(i,s,o);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), foo_sql(11,10+r);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), foo_sql(11,10+r) WITH ORDINALITY AS f(i,s,o);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (11,12),(13,15),(16,20)) v(r1,r2), foo_sql(r1,r2);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (11,12),(13,15),(16,20)) v(r1,r2), foo_sql(r1,r2) WITH ORDINALITY AS f(i,s,o);

SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), foo_mat(10+r,13);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), foo_mat(10+r,13) WITH ORDINALITY AS f(i,s,o);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), foo_mat(11,10+r);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), foo_mat(11,10+r) WITH ORDINALITY AS f(i,s,o);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (11,12),(13,15),(16,20)) v(r1,r2), foo_mat(r1,r2);
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (11,12),(13,15),(16,20)) v(r1,r2), foo_mat(r1,r2) WITH ORDINALITY AS f(i,s,o);

-- selective rescan of multiple functions:

SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), ROWS FROM( foo_sql(11,11), foo_mat(10+r,13) );
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), ROWS FROM( foo_sql(10+r,13), foo_mat(11,11) );
SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM (VALUES (1),(2),(3)) v(r), ROWS FROM( foo_sql(10+r,13), foo_mat(10+r,13) );

SELECT setval('foo_rescan_seq1',1,false),setval('foo_rescan_seq2',1,false);
SELECT * FROM generate_series(1,2) r1, generate_series(r1,3) r2, ROWS FROM( foo_sql(10+r1,13), foo_mat(10+r2,13) );

SELECT * FROM (VALUES (1),(2),(3)) v(r), generate_series(10+r,20-r) f(i);
SELECT * FROM (VALUES (1),(2),(3)) v(r), generate_series(10+r,20-r) WITH ORDINALITY AS f(i,o);

SELECT * FROM (VALUES (1),(2),(3)) v(r), unnest(array[r*10,r*20,r*30]) f(i);
SELECT * FROM (VALUES (1),(2),(3)) v(r), unnest(array[r*10,r*20,r*30]) WITH ORDINALITY AS f(i,o);

-- deep nesting

SELECT * FROM (VALUES (1),(2),(3)) v1(r1),
              LATERAL (SELECT r1, * FROM (VALUES (10),(20),(30)) v2(r2)
                                         LEFT JOIN generate_series(21,23) f(i) ON ((r2+i)<100) OFFSET 0) s1;
SELECT * FROM (VALUES (1),(2),(3)) v1(r1),
              LATERAL (SELECT r1, * FROM (VALUES (10),(20),(30)) v2(r2)
                                         LEFT JOIN generate_series(20+r1,23) f(i) ON ((r2+i)<100) OFFSET 0) s1;
SELECT * FROM (VALUES (1),(2),(3)) v1(r1),
              LATERAL (SELECT r1, * FROM (VALUES (10),(20),(30)) v2(r2)
                                         LEFT JOIN generate_series(r2,r2+3) f(i) ON ((r2+i)<100) OFFSET 0) s1;
SELECT * FROM (VALUES (1),(2),(3)) v1(r1),
              LATERAL (SELECT r1, * FROM (VALUES (10),(20),(30)) v2(r2)
                                         LEFT JOIN generate_series(r1,2+r2/5) f(i) ON ((r2+i)<100) OFFSET 0) s1;

DROP FUNCTION foo_sql(int,int);
DROP FUNCTION foo_mat(int,int);
DROP SEQUENCE foo_rescan_seq1;
DROP SEQUENCE foo_rescan_seq2;

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

-- fails, as we are attempting to rename first argument
CREATE OR REPLACE FUNCTION dup (inout f2 anyelement, out f3 anyarray)
AS 'select $1, array[$1,$1]' LANGUAGE sql;

DROP FUNCTION dup(anyelement);

-- equivalent behavior, though different name exposed for input arg
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

-- case that causes change of typmod knowledge during inlining
CREATE OR REPLACE FUNCTION foo()
RETURNS TABLE(a varchar(5))
AS $$ SELECT 'hello'::varchar(5) $$ LANGUAGE sql STABLE;
SELECT * FROM foo() GROUP BY 1;
DROP FUNCTION foo();

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
-- Check some cases involving added/dropped columns in a rowtype result
--

create temp table users (userid text, seq int, email text, todrop bool, moredrop int, enabled bool);
insert into users values ('id',1,'email',true,11,true);
insert into users values ('id2',2,'email2',true,12,true);
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
SELECT * FROM get_users() WITH ORDINALITY;   -- make sure ordinality copes

-- multiple functions vs. dropped columns
SELECT * FROM ROWS FROM(generate_series(10,11), get_users()) WITH ORDINALITY;
SELECT * FROM ROWS FROM(get_users(), generate_series(10,11)) WITH ORDINALITY;

-- check that we can cope with post-parsing changes in rowtypes
create temp view usersview as
SELECT * FROM ROWS FROM(get_users(), generate_series(10,11)) WITH ORDINALITY;

select * from usersview;
alter table users drop column moredrop;
select * from usersview;
alter table users add column junk text;
select * from usersview;
alter table users alter column seq type numeric;
select * from usersview;  -- expect clean failure

drop view usersview;
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

-- check handling of a SQL function with multiple OUT params (bug #5777)

create or replace function foobar(out integer, out numeric) as
$$ select (1, 2.1) $$ language sql;

select * from foobar();

create or replace function foobar(out integer, out numeric) as
$$ select (1, 2) $$ language sql;

select * from foobar();  -- fail

create or replace function foobar(out integer, out numeric) as
$$ select (1, 2.1, 3) $$ language sql;

select * from foobar();  -- fail

drop function foobar();

-- check behavior when a function's input sometimes returns a set (bug #8228)

SELECT *,
  lower(CASE WHEN id = 2 THEN (regexp_matches(str, '^0*([1-9]\d+)$'))[1]
        ELSE str
        END)
FROM
  (VALUES (1,''), (2,'0000000049404'), (3,'FROM 10000000876')) v(id, str);

-- check whole-row-Var handling in nested lateral functions (bug #11703)

create function extractq2(t int8_tbl) returns int8 as $$
  select t.q2
$$ language sql immutable;

explain (verbose, costs off)
select x from int8_tbl, extractq2(int8_tbl) f(x);

select x from int8_tbl, extractq2(int8_tbl) f(x);

create function extractq2_2(t int8_tbl) returns table(ret1 int8) as $$
  select extractq2(t) offset 0
$$ language sql immutable;

explain (verbose, costs off)
select x from int8_tbl, extractq2_2(int8_tbl) f(x);

select x from int8_tbl, extractq2_2(int8_tbl) f(x);

-- without the "offset 0", this function gets optimized quite differently

create function extractq2_2_opt(t int8_tbl) returns table(ret1 int8) as $$
  select extractq2(t)
$$ language sql immutable;

explain (verbose, costs off)
select x from int8_tbl, extractq2_2_opt(int8_tbl) f(x);

select x from int8_tbl, extractq2_2_opt(int8_tbl) f(x);
