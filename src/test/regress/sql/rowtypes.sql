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
