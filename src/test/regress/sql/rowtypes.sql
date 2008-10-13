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

begin;
set local add_missing_from = false;
select f1, q.c1 from quadtable;		-- fails, q is a table reference
rollback;

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

-- Check row comparison with a subselect
select unique1, unique2 from tenk1
where (unique1, unique2) < any (select ten, ten from tenk1 where hundred < 3)
      and unique1 <= 20
order by 1;

-- Also check row comparison with an indexable condition
select thousand, tenthous from tenk1
where (thousand, tenthous) >= (997, 5000)
order by thousand, tenthous;

-- Check some corner cases involving empty rowtypes
select ROW();
select ROW() IS NULL;
select ROW() = ROW();

-- Check ability to create arrays of anonymous rowtypes
select array[ row(1,2), row(3,4), row(5,6) ];

-- Check ability to compare an anonymous row to elements of an array
select row(1,1.1) = any (array[ row(7,7.7), row(1,1.1), row(0,0.0) ]);
select row(1,1.1) = any (array[ row(7,7.7), row(1,1.0), row(0,0.0) ]);
