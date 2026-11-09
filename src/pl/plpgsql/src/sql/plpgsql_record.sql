--
-- Tests for PL/pgSQL handling of composite (record) variables
--

create type two_int4s as (f1 int4, f2 int4);
create type more_int4s as (f0 text, f1 int4, f2 int4);
create type two_int8s as (q1 int8, q2 int8);
create type nested_int8s as (c1 two_int8s, c2 two_int8s);

-- base-case return of a composite type
create function retc(int) returns two_int8s language plpgsql as
$$ begin return row($1,1)::two_int8s; end $$;
select retc(42);

-- ok to return a matching record type
create or replace function retc(int) returns two_int8s language plpgsql as
$$ begin return row($1::int8, 1::int8); end $$;
select retc(42);

-- we don't currently support implicit casting
create or replace function retc(int) returns two_int8s language plpgsql as
$$ begin return row($1,1); end $$;
select retc(42);

-- nor extra columns
create or replace function retc(int) returns two_int8s language plpgsql as
$$ begin return row($1::int8, 1::int8, 42); end $$;
select retc(42);

-- same cases with an intermediate "record" variable
create or replace function retc(int) returns two_int8s language plpgsql as
$$ declare r record; begin r := row($1::int8, 1::int8); return r; end $$;
select retc(42);

create or replace function retc(int) returns two_int8s language plpgsql as
$$ declare r record; begin r := row($1,1); return r; end $$;
select retc(42);

create or replace function retc(int) returns two_int8s language plpgsql as
$$ declare r record; begin r := row($1::int8, 1::int8, 42); return r; end $$;
select retc(42);

-- but, for mostly historical reasons, we do convert when assigning
-- to a named-composite-type variable
create or replace function retc(int) returns two_int8s language plpgsql as
$$ declare r two_int8s; begin r := row($1::int8, 1::int8, 42); return r; end $$;
select retc(42);

do $$ declare c two_int8s;
begin c := row(1,2); raise notice 'c = %', c; end$$;

do $$ declare c two_int8s;
begin for c in select 1,2 loop raise notice 'c = %', c; end loop; end$$;

do $$ declare c4 two_int4s; c8 two_int8s;
begin
  c8 := row(1,2);
  c4 := c8;
  c8 := c4;
  raise notice 'c4 = %', c4;
  raise notice 'c8 = %', c8;
end$$;

do $$ declare c two_int8s; d nested_int8s;
begin
  c := row(1,2);
  d := row(c, row(c.q1, c.q2+1));
  raise notice 'c = %, d = %', c, d;
  c.q1 := 10;
  d.c1 := row(11,12);
  d.c2.q2 := 42;
  raise notice 'c = %, d = %', c, d;
  raise notice 'c.q1 = %, d.c2 = %', c.q1, d.c2;
  raise notice '(d).c2.q2 = %', (d).c2.q2;  -- doesn't work without parens
  raise notice '(d.c2).q2 = %', (d.c2).q2;  -- doesn't work without parens
end$$;

-- block-qualified naming
do $$ <<b>> declare c two_int8s; d nested_int8s;
begin
  b.c := row(1,2);
  b.d := row(b.c, row(b.c.q1, b.c.q2+1));
  raise notice 'b.c = %, b.d = %', b.c, b.d;
  b.c.q1 := 10;
  b.d.c1 := row(11,12);
  b.d.c2.q2 := 42;
  raise notice 'b.c = %, b.d = %', b.c, b.d;
  raise notice 'b.c.q1 = %, b.d.c2 = %', b.c.q1, b.d.c2;
  raise notice '(b.d).c2.q2 = %', (b.d).c2.q2;  -- doesn't work without parens
  raise notice '(b.d.c2).q2 = %', (b.d.c2).q2;  -- doesn't work without parens
end$$;

-- error cases
do $$ declare c two_int8s; begin c.x = 1; end $$;
do $$ declare c nested_int8s; begin c.x = 1; end $$;
do $$ declare c nested_int8s; begin c.x.q1 = 1; end $$;
do $$ declare c nested_int8s; begin c.c2.x = 1; end $$;
do $$ declare c nested_int8s; begin d.c2.x = 1; end $$;
do $$ <<b>> declare c two_int8s; begin b.c.x = 1; end $$;
do $$ <<b>> declare c nested_int8s; begin b.c.x = 1; end $$;
do $$ <<b>> declare c nested_int8s; begin b.c.x.q1 = 1; end $$;
do $$ <<b>> declare c nested_int8s; begin b.c.c2.x = 1; end $$;
do $$ <<b>> declare c nested_int8s; begin b.d.c2.x = 1; end $$;

-- check passing composite result to another function
create function getq1(two_int8s) returns int8 language plpgsql as $$
declare r two_int8s; begin r := $1; return r.q1; end $$;

select getq1(retc(344));
select getq1(row(1,2));

do $$
declare r1 two_int8s; r2 record; x int8;
begin
  r1 := retc(345);
  perform getq1(r1);
  x := getq1(r1);
  raise notice 'x = %', x;
  r2 := retc(346);
  perform getq1(r2);
  x := getq1(r2);
  raise notice 'x = %', x;
end$$;

-- check assignments of composites
do $$
declare r1 two_int8s; r2 two_int8s; r3 record; r4 record;
begin
  r1 := row(1,2);
  raise notice 'r1 = %', r1;
  r1 := r1;  -- shouldn't do anything
  raise notice 'r1 = %', r1;
  r2 := r1;
  raise notice 'r1 = %', r1;
  raise notice 'r2 = %', r2;
  r2.q2 = r1.q1 + 3;  -- check that r2 has distinct storage
  raise notice 'r1 = %', r1;
  raise notice 'r2 = %', r2;
  r1 := null;
  raise notice 'r1 = %', r1;
  raise notice 'r2 = %', r2;
  r1 := row(7,11)::two_int8s;
  r2 := r1;
  raise notice 'r1 = %', r1;
  raise notice 'r2 = %', r2;
  r3 := row(1,2);
  r4 := r3;
  raise notice 'r3 = %', r3;
  raise notice 'r4 = %', r4;
  r4.f1 := r4.f1 + 3;  -- check that r4 has distinct storage
  raise notice 'r3 = %', r3;
  raise notice 'r4 = %', r4;
  r1 := r3;
  raise notice 'r1 = %', r1;
  r4 := r1;
  raise notice 'r4 = %', r4;
  r4.q2 := r4.q2 + 1;  -- r4's field names have changed
  raise notice 'r4 = %', r4;
end$$;

-- fields of named-type vars read as null if uninitialized
do $$
declare r1 two_int8s;
begin
  raise notice 'r1 = %', r1;
  raise notice 'r1.q1 = %', r1.q1;
  raise notice 'r1.q2 = %', r1.q2;
  raise notice 'r1 = %', r1;
end$$;

do $$
declare r1 two_int8s;
begin
  raise notice 'r1.q1 = %', r1.q1;
  raise notice 'r1.q2 = %', r1.q2;
  raise notice 'r1 = %', r1;
  raise notice 'r1.nosuchfield = %', r1.nosuchfield;
end$$;

-- records, not so much
do $$
declare r1 record;
begin
  raise notice 'r1 = %', r1;
  raise notice 'r1.f1 = %', r1.f1;
  raise notice 'r1.f2 = %', r1.f2;
  raise notice 'r1 = %', r1;
end$$;

-- but OK if you assign first
do $$
declare r1 record;
begin
  raise notice 'r1 = %', r1;
  r1 := row(1,2);
  raise notice 'r1.f1 = %', r1.f1;
  raise notice 'r1.f2 = %', r1.f2;
  raise notice 'r1 = %', r1;
  raise notice 'r1.nosuchfield = %', r1.nosuchfield;
end$$;

-- check %type with block-qualified variable names
do $$
<<blk>>
declare
  v int;
  r two_int8s;
  v1 v%type;
  v2 blk.v%type;
  r1 r%type;
  r2 blk.r%type;
begin
  raise notice '%', pg_typeof(v1);
  raise notice '%', pg_typeof(v2);
  raise notice '%', pg_typeof(r1);
  raise notice '%', pg_typeof(r2);
end$$;

-- check that type record can be passed through %type
do $$
declare r1 record;
        r2 r1%type;
begin
  r2 := row(1,2);
  raise notice 'r2 = %', r2;
  r2 := row(3,4,5);
  raise notice 'r2 = %', r2;
end$$;

-- arrays of record are not supported at the moment
do $$
declare r1 record[];
begin
end$$;

do $$
declare r1 record;
        r2 r1%type[];
begin
end$$;

-- check repeated assignments to composite fields
create table some_table (id int, data text);

do $$
declare r some_table;
begin
  r := (23, 'skidoo');
  for i in 1 .. 10 loop
    r.id := r.id + i;
    r.data := r.data || ' ' || i;
  end loop;
  raise notice 'r = %', r;
end$$;

-- check behavior of function declared to return "record"

create function returnsrecord(int) returns record language plpgsql as
$$ begin return row($1,$1+1); end $$;

select returnsrecord(42);
select * from returnsrecord(42) as r(x int, y int);
select * from returnsrecord(42) as r(x int, y int, z int);  -- fail
select * from returnsrecord(42) as r(x int, y bigint);  -- fail

-- same with an intermediate record variable
create or replace function returnsrecord(int) returns record language plpgsql as
$$ declare r record; begin r := row($1,$1+1); return r; end $$;

select returnsrecord(42);
select * from returnsrecord(42) as r(x int, y int);
select * from returnsrecord(42) as r(x int, y int, z int);  -- fail
select * from returnsrecord(42) as r(x int, y bigint);  -- fail

-- should work the same with a missing column in the actual result value
create table has_hole(f1 int, f2 int, f3 int);
alter table has_hole drop column f2;

create or replace function returnsrecord(int) returns record language plpgsql as
$$ begin return row($1,$1+1)::has_hole; end $$;

select returnsrecord(42);
select * from returnsrecord(42) as r(x int, y int);
select * from returnsrecord(42) as r(x int, y int, z int);  -- fail
select * from returnsrecord(42) as r(x int, y bigint);  -- fail

-- same with an intermediate record variable
create or replace function returnsrecord(int) returns record language plpgsql as
$$ declare r record; begin r := row($1,$1+1)::has_hole; return r; end $$;

select returnsrecord(42);
select * from returnsrecord(42) as r(x int, y int);
select * from returnsrecord(42) as r(x int, y int, z int);  -- fail
select * from returnsrecord(42) as r(x int, y bigint);  -- fail

-- check access to a field of an argument declared "record"
create function getf1(x record) returns int language plpgsql as
$$ begin return x.f1; end $$;
select getf1(1);
select getf1(row(1,2));
select getf1(row(1,2)::two_int4s);
select getf1(row('foo',123,456)::more_int4s);
-- the context stack is different when debug_discard_caches
-- is set, so suppress context output
\set SHOW_CONTEXT never
select getf1(row(1,2)::two_int8s);
\set SHOW_CONTEXT errors
select getf1(row(1,2));

-- this seemingly-equivalent case behaves a bit differently,
-- because the core parser's handling of $N symbols is simplistic
create function getf2(record) returns int language plpgsql as
$$ begin return $1.f2; end $$;
select getf2(row(1,2));  -- ideally would work, but does not
select getf2(row(1,2)::two_int4s);
select getf2(row('foo',123,456)::more_int4s);

-- check behavior when assignment to FOR-loop variable requires coercion
do $$
declare r two_int8s;
begin
  for r in select i, i+1 from generate_series(1,4) i
  loop
    raise notice 'r = %', r;
  end loop;
end$$;

-- check behavior when returning setof composite
create function returnssetofholes() returns setof has_hole language plpgsql as
$$
declare r record;
  h has_hole;
begin
  return next h;
  r := (1,2);
  h := (3,4);
  return next r;
  return next h;
  return next row(5,6);
  return next row(7,8)::has_hole;
  return query select 9, 10;
end$$;
select returnssetofholes();

create or replace function returnssetofholes() returns setof has_hole language plpgsql as
$$
declare r record;
begin
  return next r;  -- fails, not assigned yet
end$$;
select returnssetofholes();

create or replace function returnssetofholes() returns setof has_hole language plpgsql as
$$
begin
  return next row(1,2,3);  -- fails
end$$;
select returnssetofholes();

create or replace function returnssetofholes() returns setof has_hole language plpgsql as
$$
begin
  return query select 1, 2.0;  -- fails
end$$;
select returnssetofholes();

-- check behavior with changes of a named rowtype
create table mutable(f1 int, f2 text);

create function sillyaddone(int) returns int language plpgsql as
$$ declare r mutable; begin r.f1 := $1; return r.f1 + 1; end $$;
select sillyaddone(42);

-- test for change of type of column f1 should be here someday;
-- for now see plpgsql_cache test

alter table mutable drop column f1;
-- the context stack is different when debug_discard_caches
-- is set, so suppress context output
\set SHOW_CONTEXT never
select sillyaddone(42);  -- fail
\set SHOW_CONTEXT errors

create function getf3(x mutable) returns int language plpgsql as
$$ begin return x.f3; end $$;
select getf3(null::mutable);  -- doesn't work yet
alter table mutable add column f3 int;
select getf3(null::mutable);  -- now it works
alter table mutable drop column f3;
-- the context stack is different when debug_discard_caches
-- is set, so suppress context output
\set SHOW_CONTEXT never
select getf3(null::mutable);  -- fails again
\set SHOW_CONTEXT errors

-- check behavior with creating/dropping a named rowtype
set check_function_bodies = off;  -- else reference to nonexistent type fails

create function sillyaddtwo(int) returns int language plpgsql as
$$ declare r mutable2; begin r.f1 := $1; return r.f1 + 2; end $$;

reset check_function_bodies;

select sillyaddtwo(42);  -- fail
create table mutable2(f1 int, f2 text);
select sillyaddtwo(42);
drop table mutable2;
-- the context stack is different when debug_discard_caches
-- is set, so suppress context output
\set SHOW_CONTEXT never
select sillyaddtwo(42);  -- fail
\set SHOW_CONTEXT errors
create table mutable2(f0 text, f1 int, f2 text);
select sillyaddtwo(42);
select sillyaddtwo(43);


-- check that a record variable's declared type is re-resolved by name, so a
-- variable can come to mean a different type while the function is running.
-- The re-resolution happens only once the type cache entry the variable was
-- compiled against has been invalidated, so which of the two results below
-- you get depends on whether something invalidated it in the meantime.

create schema recmut1;
create schema recmut2;
create type recmut1.rtype as (a int);

-- Without an invalidation the variable keeps the type it was compiled with,
-- and assigning the wider value silently drops the extra column.  Pin
-- debug_discard_caches, otherwise this case does not reach the assignment
-- with a valid type cache entry.
set debug_discard_caches = 0;
set search_path = recmut1, public;
do $$
declare r rtype;                               -- compiled against recmut1.rtype
begin
  create type recmut2.rtype as (a int, b int);
  set search_path = recmut2, recmut1, public;  -- "rtype" now names recmut2.rtype
  r := row(2,3)::recmut2.rtype;
  raise notice 'without invalidation: %', r;
end$$;
reset search_path;
reset debug_discard_caches;
drop type recmut2.rtype;

-- With one, the same code follows the name to the other type instead.
set search_path = recmut1, public;
do $$
declare r rtype;
begin
  create type recmut2.rtype as (a int, b int);
  set search_path = recmut2, recmut1, public;
  alter type recmut1.rtype rename attribute a to a2;
  alter type recmut1.rtype rename attribute a2 to a;
  r := row(2,3)::recmut2.rtype;
  raise notice 'with invalidation:    %', r;
end$$;
reset search_path;

drop schema recmut2 cascade;
drop schema recmut1 cascade;

-- Assignment between different descriptor vintages.  When the attribute types
-- and tuple layout are unchanged, it would be OK for this to work.

-- Copy a source with the old attribute name into a target with the new name.
create type recmove as (a int);
do $$
declare s recmove; d recmove;
begin
  s := row(1)::recmove;
  alter type recmove rename attribute a to renamed;
  d := row(2)::recmove;
  d := s;
  raise notice 'd.renamed = %', d.renamed;
end$$;
drop type recmove;

-- Assign a current expanded record into a target initialized before a no-op
-- type alteration.
create type recmove as (a int);
do $$
declare r1 recmove; r2 recmove;
begin
  r2 := row(1)::recmove;
  alter type recmove alter attribute a type int;
  r1 := row(2)::recmove;
  r2 := r1;
  raise notice 'r2.a = %', r2.a;
end$$;
drop type recmove;

-- Same, with a plain composite datum as the source.
create type recmove as (a int);
do $$
declare r recmove;
begin
  r := row(1)::recmove;
  alter type recmove alter attribute a type int;
  r := row(2)::recmove;
  raise notice 'r.a = %', r.a;
end$$;
drop type recmove;

-- Copy an executor-derived record with an older descriptor, then assign a
-- fresh datum.  The first copy must not prevent the subsequent assignment.
create type recmove as (a int);
create table recmove_src (a int);
insert into recmove_src values (1);
do $$
declare r1 recmove; r2 recmove;
begin
  select * into r1 from recmove_src;
  alter type recmove alter attribute a type int;
  r2 := row(2)::recmove;
  r2 := r1;
  r2 := row(3)::recmove;
  raise notice 'r2.a = %', r2.a;
end$$;
drop table recmove_src;
drop type recmove;

-- Adding or dropping a column changes the layout, so assignment between
-- variables set before and after the change may fail.
create table recmove_tab (f1 int, f2 int);
insert into recmove_tab values (1, 2);
do $$
declare r1 recmove_tab; r2 recmove_tab;
begin
  select * into r1 from recmove_tab;
  alter table recmove_tab add column f3 int;
  r2 := row(3, 4, 5)::recmove_tab;
  r2 := r1;
  raise notice 'r2 = %', r2;
end$$;
drop table recmove_tab;

create table recmove_tab (f1 int, f2 int);
insert into recmove_tab values (1, 2);
do $$
declare r1 recmove_tab; r2 recmove_tab;
begin
  select * into r1 from recmove_tab;
  alter table recmove_tab drop column f2;
  r2 := row(3)::recmove_tab;
  r2 := r1;
  raise notice 'r2 = %', r2;
end$$;
drop table recmove_tab;

-- check access to system columns in a record variable

create function sillytrig() returns trigger language plpgsql as
$$begin
  raise notice 'old.ctid = %', old.ctid;
  raise notice 'old.tableoid = %', old.tableoid::regclass;
  return new;
end$$;

create trigger mutable_trig before update on mutable for each row
execute procedure sillytrig();

insert into mutable values ('foo'), ('bar');
update mutable set f2 = f2 || ' baz';
table mutable;


-- check that system columns survive assignment between record variables only
-- when the source tuple can be copied verbatim; where a conversion is needed
-- they are silently dropped.  For a domain source that depends on whether the
-- composite type was invalidated between the two statements, so those cases
-- are run both ways.  debug_discard_caches is pinned so that the ANALYZE is
-- the only invalidation, and each ANALYZE case gets its own table because
-- ANALYZE only invalidates when it actually changes the statistics.

set debug_discard_caches = 0;

create table sysmove(f1 int);
insert into sysmove values (1);
create domain sysmoved as sysmove;

do $$
declare s sysmove; d sysmove;
begin
  select * into s from sysmove;
  d := s;
  raise notice 'rowtype -> rowtype: src %, dst %', s.ctid, d.ctid;
end$$;

do $$
declare s record; d sysmove;
begin
  select * into s from sysmove;
  d := s;
  raise notice 'record  -> rowtype: src %, dst %', s.ctid, d.ctid;
end$$;

do $$
declare s sysmoved; d sysmove;
begin
  select * into s from sysmove;
  d := s;
  raise notice 'domain  -> rowtype: src %, dst %', s.ctid, d.ctid;
end$$;

do $$
declare s sysmoved; d record;
begin
  select * into s from sysmove;
  d := s;
  raise notice 'domain  -> record:  src %, dst %', s.ctid, d.ctid;
end$$;

create table sysmove2(f1 int);
insert into sysmove2 values (1);
create domain sysmoved2 as sysmove2;

do $$
declare s sysmoved2; d sysmove2;
begin
  select * into s from sysmove2;
  analyze sysmove2;
  d := s;
  raise notice 'domain  -> rowtype, invalidated: src %, dst %', s.ctid, d.ctid;
end$$;

create table sysmove3(f1 int);
insert into sysmove3 values (1);
create domain sysmoved3 as sysmove3;

do $$
declare s sysmoved3; d record;
begin
  select * into s from sysmove3;
  analyze sysmove3;
  d := s;
  raise notice 'domain  -> record,  invalidated: src %, dst %', s.ctid, d.ctid;
end$$;

-- A rename round trip also invalidates the descriptor without changing the
-- row type.  Copying the tuple should still preserve its system columns.
do $$
declare s sysmoved; d sysmove;
begin
  select * into s from sysmove;
  alter table sysmove rename column f1 to f2;
  alter table sysmove rename column f2 to f1;
  d := s;
  raise notice 'domain  -> rowtype, renamed: src %, dst %', s.ctid, d.ctid;
end$$;

drop domain sysmoved, sysmoved2, sysmoved3;
drop table sysmove, sysmove2, sysmove3;
reset debug_discard_caches;

-- Preserve system columns when assigning a rowtype parameter whose declared
-- type name now refers to a different table.
create table sysparam (x int);
insert into sysparam values (1);
create function sysparam_probe(s sysparam) returns boolean language plpgsql as $$
declare d sysparam;
begin
  select * into d from sysparam;
  alter table sysparam rename to sysparam_old;
  create table sysparam (x int8);
  select * into s from sysparam_old;
  d := s;
  raise notice 'tableoid kept: %, ctid kept: %',
    d.tableoid is not null, d.ctid is not null;
  return d.ctid is not null;
end$$;
select sysparam_probe(null::sysparam);
drop function sysparam_probe(sysparam_old);
drop table sysparam, sysparam_old;

-- Check behavior when using variables used before/after attribute rename, a
-- harmless operation.
CREATE TYPE altered_type AS (a int);
DO $$
DECLARE
  -- variable first assigned before DDL and then again after
  r_again_after altered_type;
  -- variable assigned before DDL
  r_before altered_type;
  -- variable assigned after DDL
  r_after altered_type;
BEGIN
  r_again_after := ROW(1)::altered_type;
  r_before = ROW(2)::altered_type;
  ALTER TYPE altered_type RENAME ATTRIBUTE a TO b;

  BEGIN
    RAISE NOTICE 'r_before: %', r_before;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r_before failed: %', sqlerrm;
  END;

  BEGIN
    r_again_after := ROW(3)::altered_type;
    RAISE NOTICE 'r_again_after: %', r_again_after;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r_again_after failed: %', sqlerrm;
  END;

  BEGIN
    r_after = ROW(4)::altered_type;
    RAISE NOTICE 'r_after: %', r_after;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r_after failed: %', sqlerrm;
  END;
END $$;
DROP TYPE altered_type;

-- Check behavior when using variables used before/after SET TYPE that doesn't
-- actually change the type.
CREATE TYPE altered_type AS (a int);
DO $$
DECLARE
  -- variable first assigned before DDL and then again after
  r_again_after altered_type;
  -- variable assigned before DDL
  r_before altered_type;
  -- variable assigned after DDL
  r_after altered_type;
BEGIN
  r_again_after := ROW(1)::altered_type;
  r_before = ROW(2)::altered_type;
  ALTER TYPE altered_type ALTER ATTRIBUTE a TYPE int;

  BEGIN
    RAISE NOTICE 'r_before: %', r_before;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r_before failed: %', sqlerrm;
  END;

  BEGIN
    r_again_after := ROW(3)::altered_type;
    RAISE NOTICE 'r_again_after: %', r_again_after;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r_again_after failed: %', sqlerrm;
  END;

  BEGIN
    r_after = ROW(4)::altered_type;
    RAISE NOTICE 'r_after: %', r_after;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r_after failed: %', sqlerrm;
  END;
END $$;
DROP TYPE altered_type;


-- Check behavior when using variables used before/after SET TYPE that does
-- actually change the type.
CREATE TYPE altered_type AS (a bytea);
DO $$
DECLARE
  -- variable first assigned before DDL and then again after
  r_again_after altered_type;
  -- variable assigned before DDL
  r_before altered_type;
  -- variable assigned after DDL
  r_after altered_type;
BEGIN
  r_again_after := ROW('1')::altered_type;
  r_before = ROW('2')::altered_type;
  ALTER TYPE altered_type ALTER ATTRIBUTE a TYPE text;

  BEGIN
    RAISE NOTICE 'r_before: %', r_before;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r_before failed: %', sqlerrm;
  END;

  BEGIN
    r_again_after := ROW(3)::altered_type;
    RAISE NOTICE 'r_again_after: %', r_again_after;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r_again_after failed: %', sqlerrm;
  END;

  BEGIN
    r_after = ROW(4)::altered_type;
    RAISE NOTICE 'r_after: %', r_after;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r_after failed: %', sqlerrm;
  END;
END $$;
DROP TYPE altered_type;


-- check returning a composite datum from a trigger

create or replace function sillytrig() returns trigger language plpgsql as
$$begin
  return row(new.*);
end$$;

update mutable set f2 = f2 || ' baz';
table mutable;

create or replace function sillytrig() returns trigger language plpgsql as
$$declare r record;
begin
  r := row(new.*);
  return r;
end$$;

update mutable set f2 = f2 || ' baz';
table mutable;

--
-- Domains of composite
--

create domain ordered_int8s as two_int8s check((value).q1 <= (value).q2);

create function read_ordered_int8s(p ordered_int8s) returns int8 as $$
begin return p.q1 + p.q2; end
$$ language plpgsql;

select read_ordered_int8s(row(1, 2));
select read_ordered_int8s(row(2, 1));  -- fail

create function build_ordered_int8s(i int8, j int8) returns ordered_int8s as $$
begin return row(i,j); end
$$ language plpgsql;

select build_ordered_int8s(1,2);
select build_ordered_int8s(2,1);  -- fail

create function build_ordered_int8s_2(i int8, j int8) returns ordered_int8s as $$
declare r record; begin r := row(i,j); return r; end
$$ language plpgsql;

select build_ordered_int8s_2(1,2);
select build_ordered_int8s_2(2,1);  -- fail

create function build_ordered_int8s_3(i int8, j int8) returns ordered_int8s as $$
declare r two_int8s; begin r := row(i,j); return r; end
$$ language plpgsql;

select build_ordered_int8s_3(1,2);
select build_ordered_int8s_3(2,1);  -- fail

create function build_ordered_int8s_4(i int8, j int8) returns ordered_int8s as $$
declare r ordered_int8s; begin r := row(i,j); return r; end
$$ language plpgsql;

select build_ordered_int8s_4(1,2);
select build_ordered_int8s_4(2,1);  -- fail

create function build_ordered_int8s_a(i int8, j int8) returns ordered_int8s[] as $$
begin return array[row(i,j), row(i,j+1)]; end
$$ language plpgsql;

select build_ordered_int8s_a(1,2);
select build_ordered_int8s_a(2,1);  -- fail

-- check field assignment
do $$
declare r ordered_int8s;
begin
  r.q1 := null;
  r.q2 := 43;
  r.q1 := 42;
  r.q2 := 41;  -- fail
end$$;

-- check whole-row assignment
do $$
declare r ordered_int8s;
begin
  r := null;
  r := row(null,null);
  r := row(1,2);
  r := row(2,1);  -- fail
end$$;

-- check assignment in for-loop
do $$
declare r ordered_int8s;
begin
  for r in values (1,2),(3,4),(6,5) loop
    raise notice 'r = %', r;
  end loop;
end$$;

-- check behavior with toastable fields, too

create type two_texts as (f1 text, f2 text);
create domain ordered_texts as two_texts check((value).f1 <= (value).f2);

create table sometable (id int, a text, b text);
-- b should be compressed, but in-line
insert into sometable values (1, 'a', repeat('ffoob',1000));
-- this b should be out-of-line
insert into sometable values (2, 'a', repeat('ffoob',100000));
-- this pair should fail the domain check
insert into sometable values (3, 'z', repeat('ffoob',100000));

do $$
declare d ordered_texts;
begin
  for d in select a, b from sometable loop
    raise notice 'succeeded at "%"', d.f1;
  end loop;
end$$;

do $$
declare r record; d ordered_texts;
begin
  for r in select * from sometable loop
    raise notice 'processing row %', r.id;
    d := row(r.a, r.b);
  end loop;
end$$;

do $$
declare r record; d ordered_texts;
begin
  for r in select * from sometable loop
    raise notice 'processing row %', r.id;
    d := null;
    d.f1 := r.a;
    d.f2 := r.b;
  end loop;
end$$;

-- check coercion of a record result to named-composite function output type
create function compresult(int8) returns two_int8s language plpgsql as
$$ declare r record; begin r := row($1,$1); return r; end $$;

create table two_int8s_tab (f1 two_int8s);
insert into two_int8s_tab values (compresult(42));
-- reconnect so we lose any local knowledge of anonymous record types
\c -
table two_int8s_tab;
