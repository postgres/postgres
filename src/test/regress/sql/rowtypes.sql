--
-- ROWTYPES
--

-- Make both a standalone composite type and a table rowtype

create type complex as (r float8, i float8);

create temp table fullname (first text, last text);

-- Nested composite

create type quad as (c1 complex, c2 complex);

-- Some simple tests of I/O conversions and row construction

select (1.1,2.2)::complex, row((3.3,4.4),(5.5,null))::quad;

select row('Joe', 'Blow')::fullname, '(Joe,Blow)'::fullname;

select '(Joe,von Blow)'::fullname, '(Joe,d''Blow)'::fullname;

select '(Joe,"von""Blow")'::fullname, E'(Joe,d\\\\Blow)'::fullname;

select '(Joe,"Blow,Jr")'::fullname;

select '(Joe,)'::fullname;	-- ok, null 2nd column
select '(Joe)'::fullname;	-- bad
select '(Joe,,)'::fullname;	-- bad

create temp table quadtable(f1 int, q quad);

insert into quadtable values (1, ((3.3,4.4),(5.5,6.6)));
insert into quadtable values (2, ((null,4.4),(5.5,6.6)));

select * from quadtable;

select f1, q.c1 from quadtable;		-- fails, q is a table reference

select f1, (q).c1, (qq.q).c1.i from quadtable qq;

create temp table people (fn fullname, bd date);

insert into people values ('(Joe,Blow)', '1984-01-10');

select * from people;

-- at the moment this will not work due to ALTER TABLE inadequacy:
alter table fullname add column suffix text default '';

-- but this should work:
alter table fullname add column suffix text default null;

select * from people;

-- test insertion/updating of subfields
update people set fn.suffix = 'Jr';

select * from people;

insert into quadtable (f1, q.c1.r, q.c2.i) values(44,55,66);

select * from quadtable;

-- The object here is to ensure that toasted references inside
-- composite values don't cause problems.  The large f1 value will
-- be toasted inside pp, it must still work after being copied to people.

create temp table pp (f1 text);
insert into pp values (repeat('abcdefghijkl', 100000));

insert into people select ('Jim', f1, null)::fullname, current_date from pp;

select (fn).first, substr((fn).last, 1, 20), length((fn).last) from people;

-- Test row comparison semantics.  Prior to PG 8.2 we did this in a totally
-- non-spec-compliant way.

select ROW(1,2) < ROW(1,3) as true;
select ROW(1,2) < ROW(1,1) as false;
select ROW(1,2) < ROW(1,NULL) as null;
select ROW(1,2,3) < ROW(1,3,NULL) as true; -- the NULL is not examined
select ROW(11,'ABC') < ROW(11,'DEF') as true;
select ROW(11,'ABC') > ROW(11,'DEF') as false;
select ROW(12,'ABC') > ROW(11,'DEF') as true;

-- = and <> have different NULL-behavior than < etc
select ROW(1,2,3) < ROW(1,NULL,4) as null;
select ROW(1,2,3) = ROW(1,NULL,4) as false;
select ROW(1,2,3) <> ROW(1,NULL,4) as true;

-- We allow operators beyond the six standard ones, if they have btree
-- operator classes.
select ROW('ABC','DEF') ~<=~ ROW('DEF','ABC') as true;
select ROW('ABC','DEF') ~>=~ ROW('DEF','ABC') as false;
select ROW('ABC','DEF') ~~ ROW('DEF','ABC') as fail;

-- Comparisons of ROW() expressions can cope with some type mismatches
select ROW(1,2) = ROW(1,2::int8);
select ROW(1,2) in (ROW(3,4), ROW(1,2));
select ROW(1,2) in (ROW(3,4), ROW(1,2::int8));

-- Check row comparison with a subselect
select unique1, unique2 from tenk1
where (unique1, unique2) < any (select ten, ten from tenk1 where hundred < 3)
      and unique1 <= 20
order by 1;

-- Also check row comparison with an indexable condition
explain (costs off)
select thousand, tenthous from tenk1
where (thousand, tenthous) >= (997, 5000)
order by thousand, tenthous;

select thousand, tenthous from tenk1
where (thousand, tenthous) >= (997, 5000)
order by thousand, tenthous;

-- Test case for bug #14010: indexed row comparisons fail with nulls
create temp table test_table (a text, b text);
insert into test_table values ('a', 'b');
insert into test_table select 'a', null from generate_series(1,1000);
insert into test_table values ('b', 'a');
create index on test_table (a,b);
set enable_sort = off;

explain (costs off)
select a,b from test_table where (a,b) > ('a','a') order by a,b;

select a,b from test_table where (a,b) > ('a','a') order by a,b;

reset enable_sort;

-- Check row comparisons with IN
select * from int8_tbl i8 where i8 in (row(123,456));  -- fail, type mismatch

explain (costs off)
select * from int8_tbl i8
where i8 in (row(123,456)::int8_tbl, '(4567890123456789,123)');

select * from int8_tbl i8
where i8 in (row(123,456)::int8_tbl, '(4567890123456789,123)');

-- Check some corner cases involving empty rowtypes
select ROW();
select ROW() IS NULL;
select ROW() = ROW();

-- Check ability to create arrays of anonymous rowtypes
select array[ row(1,2), row(3,4), row(5,6) ];

-- Check ability to compare an anonymous row to elements of an array
select row(1,1.1) = any (array[ row(7,7.7), row(1,1.1), row(0,0.0) ]);
select row(1,1.1) = any (array[ row(7,7.7), row(1,1.0), row(0,0.0) ]);

-- Check behavior with a non-comparable rowtype
create type cantcompare as (p point, r float8);
create temp table cc (f1 cantcompare);
insert into cc values('("(1,2)",3)');
insert into cc values('("(4,5)",6)');
select * from cc order by f1; -- fail, but should complain about cantcompare

--
-- Test case derived from bug #5716: check multiple uses of a rowtype result
--

BEGIN;

CREATE TABLE price (
    id SERIAL PRIMARY KEY,
    active BOOLEAN NOT NULL,
    price NUMERIC
);

CREATE TYPE price_input AS (
    id INTEGER,
    price NUMERIC
);

CREATE TYPE price_key AS (
    id INTEGER
);

CREATE FUNCTION price_key_from_table(price) RETURNS price_key AS $$
    SELECT $1.id
$$ LANGUAGE SQL;

CREATE FUNCTION price_key_from_input(price_input) RETURNS price_key AS $$
    SELECT $1.id
$$ LANGUAGE SQL;

insert into price values (1,false,42), (10,false,100), (11,true,17.99);

UPDATE price
    SET active = true, price = input_prices.price
    FROM unnest(ARRAY[(10, 123.00), (11, 99.99)]::price_input[]) input_prices
    WHERE price_key_from_table(price.*) = price_key_from_input(input_prices.*);

select * from price;

rollback;

--
-- Test case derived from bug #9085: check * qualification of composite
-- parameters for SQL functions
--

create temp table compos (f1 int, f2 text);

create function fcompos1(v compos) returns void as $$
insert into compos values (v);  -- fail
$$ language sql;

create function fcompos1(v compos) returns void as $$
insert into compos values (v.*);
$$ language sql;

create function fcompos2(v compos) returns void as $$
select fcompos1(v);
$$ language sql;

create function fcompos3(v compos) returns void as $$
select fcompos1(fcompos3.v.*);
$$ language sql;

select fcompos1(row(1,'one'));
select fcompos2(row(2,'two'));
select fcompos3(row(3,'three'));
select * from compos;

--
-- We allow I/O conversion casts from composite types to strings to be
-- invoked via cast syntax, but not functional syntax.  This is because
-- the latter is too prone to be invoked unintentionally.
--
select cast (fullname as text) from fullname;
select fullname::text from fullname;
select text(fullname) from fullname;  -- error
select fullname.text from fullname;  -- error
-- same, but RECORD instead of named composite type:
select cast (row('Jim', 'Beam') as text);
select (row('Jim', 'Beam'))::text;
select text(row('Jim', 'Beam'));  -- error
select (row('Jim', 'Beam')).text;  -- error

--
-- Test that composite values are seen to have the correct column names
-- (bug #11210 and other reports)
--

select row_to_json(i) from int8_tbl i;
select row_to_json(i) from int8_tbl i(x,y);

create temp view vv1 as select * from int8_tbl;
select row_to_json(i) from vv1 i;
select row_to_json(i) from vv1 i(x,y);

select row_to_json(ss) from
  (select q1, q2 from int8_tbl) as ss;
select row_to_json(ss) from
  (select q1, q2 from int8_tbl offset 0) as ss;
select row_to_json(ss) from
  (select q1 as a, q2 as b from int8_tbl) as ss;
select row_to_json(ss) from
  (select q1 as a, q2 as b from int8_tbl offset 0) as ss;
select row_to_json(ss) from
  (select q1 as a, q2 as b from int8_tbl) as ss(x,y);
select row_to_json(ss) from
  (select q1 as a, q2 as b from int8_tbl offset 0) as ss(x,y);

explain (costs off)
select row_to_json(q) from
  (select thousand, tenthous from tenk1
   where thousand = 42 and tenthous < 2000 offset 0) q;
select row_to_json(q) from
  (select thousand, tenthous from tenk1
   where thousand = 42 and tenthous < 2000 offset 0) q;
select row_to_json(q) from
  (select thousand as x, tenthous as y from tenk1
   where thousand = 42 and tenthous < 2000 offset 0) q;
select row_to_json(q) from
  (select thousand as x, tenthous as y from tenk1
   where thousand = 42 and tenthous < 2000 offset 0) q(a,b);

create temp table tt1 as select * from int8_tbl limit 2;
create temp table tt2 () inherits(tt1);
insert into tt2 values(0,0);
select row_to_json(r) from (select q2,q1 from tt1 offset 0) r;
