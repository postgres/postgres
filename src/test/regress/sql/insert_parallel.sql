--
-- PARALLEL
--

--
-- START: setup some tables and data needed by the tests.
--

-- Setup - index expressions test

-- For testing purposes, we'll mark this function as parallel-unsafe
create or replace function fullname_parallel_unsafe(f text, l text) returns text as $$
    begin
        return f || l;
    end;
$$ language plpgsql immutable parallel unsafe;

create or replace function fullname_parallel_restricted(f text, l text) returns text as $$
    begin
        return f || l;
    end;
$$ language plpgsql immutable parallel restricted;

create table names(index int, first_name text, last_name text);
create table names2(index int, first_name text, last_name text);
create index names2_fullname_idx on names2 (fullname_parallel_unsafe(first_name, last_name));
create table names4(index int, first_name text, last_name text);
create index names4_fullname_idx on names4 (fullname_parallel_restricted(first_name, last_name));

insert into names values
	(1, 'albert', 'einstein'),
	(2, 'niels', 'bohr'),
	(3, 'erwin', 'schrodinger'),
	(4, 'leonhard', 'euler'),
	(5, 'stephen', 'hawking'),
	(6, 'isaac', 'newton'),
	(7, 'alan', 'turing'),
	(8, 'richard', 'feynman');

-- Setup - column default tests

create or replace function bdefault_unsafe ()
returns int language plpgsql parallel unsafe as $$
begin
	RETURN 5;
end $$;

create or replace function cdefault_restricted ()
returns int language plpgsql parallel restricted as $$
begin
	RETURN 10;
end $$;

create or replace function ddefault_safe ()
returns int language plpgsql parallel safe as $$
begin
	RETURN 20;
end $$;

create table testdef(a int, b int default bdefault_unsafe(), c int default cdefault_restricted(), d int default ddefault_safe());

create table test_data(a int);
insert into test_data select * from generate_series(1,10);

--
-- END: setup some tables and data needed by the tests.
--

begin;

-- encourage use of parallel plans
set parallel_setup_cost=0;
set parallel_tuple_cost=0;
set min_parallel_table_scan_size=0;
set max_parallel_workers_per_gather=4;

create table para_insert_p1 (
	unique1		int4	PRIMARY KEY,
	stringu1	name
) with (parallel_insert_enabled = off);

create table para_insert_f1 (
	unique1		int4	REFERENCES para_insert_p1(unique1),
	stringu1	name
);

--
-- Disable guc option enable_parallel_insert
--
set enable_parallel_insert = off;

-- Test INSERT with underlying query when enable_parallel_insert=off and reloption.parallel_insert_enabled=off.
-- (should create plan with serial INSERT + SELECT)
--
explain(costs off) insert into para_insert_p1 select unique1, stringu1 from tenk1;

--
-- Reset guc option enable_parallel_insert
--
reset enable_parallel_insert;

--
-- Test INSERT with underlying query when enable_parallel_insert=on and reloption.parallel_insert_enabled=off.
-- (should create plan with serial INSERT + SELECT)
--
explain(costs off) insert into para_insert_p1 select unique1, stringu1 from tenk1;

--
-- Enable reloption parallel_insert_enabled
--
alter table para_insert_p1 set (parallel_insert_enabled = on);

--
-- Test INSERT with underlying query.
-- (should create plan with parallel SELECT, Gather parent node)
--
explain (costs off) insert into para_insert_p1 select unique1, stringu1 from tenk1;
insert into para_insert_p1 select unique1, stringu1 from tenk1;
-- select some values to verify that the parallel insert worked
select count(*), sum(unique1) from para_insert_p1;
-- verify that the same transaction has been used by all parallel workers
select count(*) from (select distinct cmin,xmin from para_insert_p1) as dt;

--
-- Test INSERT with ordered underlying query.
-- (should create plan with parallel SELECT, GatherMerge parent node)
--
truncate para_insert_p1 cascade;
explain (costs off) insert into para_insert_p1 select unique1, stringu1 from tenk1 order by unique1;
insert into para_insert_p1 select unique1, stringu1 from tenk1 order by unique1;
-- select some values to verify that the parallel insert worked
select count(*), sum(unique1) from para_insert_p1;
-- verify that the same transaction has been used by all parallel workers
select count(*) from (select distinct cmin,xmin from para_insert_p1) as dt;

--
-- Test INSERT with RETURNING clause.
-- (should create plan with parallel SELECT, Gather parent node)
--
create table test_data1(like test_data);
explain (costs off) insert into test_data1 select * from test_data where a = 10 returning a as data;
insert into test_data1 select * from test_data where a = 10 returning a as data;

--
-- Test INSERT into a table with a foreign key.
-- (Insert into a table with a foreign key is parallel-restricted,
--  as doing this in a parallel worker would create a new commandId
--  and within a worker this is not currently supported)
--
explain (costs off) insert into para_insert_f1 select unique1, stringu1 from tenk1;
insert into para_insert_f1 select unique1, stringu1 from tenk1;
-- select some values to verify that the insert worked
select count(*), sum(unique1) from para_insert_f1;

--
-- Test INSERT with ON CONFLICT ... DO UPDATE ...
-- (should not create a parallel plan)
--
create table test_conflict_table(id serial primary key, somedata int);
explain (costs off) insert into test_conflict_table(id, somedata) select a, a from test_data;
insert into test_conflict_table(id, somedata) select a, a from test_data;
explain (costs off) insert into test_conflict_table(id, somedata) select a, a from test_data ON CONFLICT(id) DO UPDATE SET somedata = EXCLUDED.somedata + 1;


--
-- Test INSERT with parallel-unsafe index expression
-- (should not create a parallel plan)
--
explain (costs off) insert into names2 select * from names;

--
-- Test INSERT with parallel-restricted index expression
-- (should create a parallel plan)
--
explain (costs off) insert into names4 select * from names;

--
-- Test INSERT with underlying query - and RETURNING (no projection)
-- (should create a parallel plan; parallel SELECT)
--
create table names5 (like names);
explain (costs off) insert into names5 select * from names returning *;

--
-- Test INSERT with underlying ordered query - and RETURNING (no projection)
-- (should create a parallel plan; parallel SELECT)
--
create table names6 (like names);
explain (costs off) insert into names6 select * from names order by last_name returning *;
insert into names6 select * from names order by last_name returning *;

--
-- Test INSERT with underlying ordered query - and RETURNING (with projection)
-- (should create a parallel plan; parallel SELECT)
--
create table names7 (like names);
explain (costs off) insert into names7 select * from names order by last_name returning last_name || ', ' || first_name as last_name_then_first_name;
insert into names7 select * from names order by last_name returning last_name || ', ' || first_name as last_name_then_first_name;


--
-- Test INSERT into temporary table with underlying query.
-- (Insert into a temp table is parallel-restricted;
-- should create a parallel plan; parallel SELECT)
--
create temporary table temp_names (like names);
explain (costs off) insert into temp_names select * from names;
insert into temp_names select * from names;

--
-- Test INSERT with column defaults
--
--

--
-- Parallel unsafe column default, should not use a parallel plan
--
explain (costs off) insert into testdef(a,c,d) select a,a*4,a*8 from test_data;

--
-- Parallel restricted column default, should use parallel SELECT
--
explain (costs off) insert into testdef(a,b,d) select a,a*2,a*8 from test_data;
insert into testdef(a,b,d) select a,a*2,a*8 from test_data;
select * from testdef order by a;
truncate testdef;

--
-- Parallel restricted and unsafe column defaults, should not use a parallel plan
--
explain (costs off) insert into testdef(a,d) select a,a*8 from test_data;

--
-- Test INSERT into partition with underlying query.
--
create table parttable1 (a int, b name) partition by range (a) with (parallel_insert_enabled=off);
create table parttable1_1 partition of parttable1 for values from (0) to (5000);
create table parttable1_2 partition of parttable1 for values from (5000) to (10000);

--
-- Test INSERT into partition when reloption.parallel_insert_enabled=off
-- (should not create a parallel plan)
--
explain (costs off) insert into parttable1 select unique1,stringu1 from tenk1;

--
-- Enable reloption parallel_insert_enabled
--
alter table parttable1 set (parallel_insert_enabled = on);

--
-- Test INSERT into partition when reloption.parallel_insert_enabled=on
-- (should create a parallel plan)
--
explain (costs off) insert into parttable1 select unique1,stringu1 from tenk1;
insert into parttable1 select unique1,stringu1 from tenk1;
select count(*) from parttable1_1;
select count(*) from parttable1_2;

--
-- Test INSERT into table with parallel-unsafe check constraint
-- (should not create a parallel plan)
--
create or replace function check_b_unsafe(b name) returns boolean as $$
    begin
        return (b <> 'XXXXXX');
    end;
$$ language plpgsql parallel unsafe;

create table table_check_b(a int4, b name check (check_b_unsafe(b)), c name);
explain (costs off) insert into table_check_b(a,b,c) select unique1, unique2, stringu1 from tenk1;

--
-- Test INSERT into table with parallel-safe after stmt-level triggers
-- (should create a parallel SELECT plan; triggers should fire)
--
create table names_with_safe_trigger (like names);
create or replace function insert_after_trigger_safe() returns trigger as $$
    begin
        raise notice 'hello from insert_after_trigger_safe';
		return new;
    end;
$$ language plpgsql parallel safe;
create trigger insert_after_trigger_safe after insert on names_with_safe_trigger
    for each statement execute procedure insert_after_trigger_safe();
explain (costs off) insert into names_with_safe_trigger select * from names;
insert into names_with_safe_trigger select * from names;

--
-- Test INSERT into table with parallel-unsafe after stmt-level triggers
-- (should not create a parallel plan; triggers should fire)
--
create table names_with_unsafe_trigger (like names);
create or replace function insert_after_trigger_unsafe() returns trigger as $$
    begin
        raise notice 'hello from insert_after_trigger_unsafe';
		return new;
    end;
$$ language plpgsql parallel unsafe;
create trigger insert_after_trigger_unsafe after insert on names_with_unsafe_trigger
    for each statement execute procedure insert_after_trigger_unsafe();
explain (costs off) insert into names_with_unsafe_trigger select * from names;
insert into names_with_unsafe_trigger select * from names;

--
-- Test INSERT into partition with parallel-unsafe trigger
-- (should not create a parallel plan)
--

create table part_unsafe_trigger (a int4, b name) partition by range (a);
create table part_unsafe_trigger_1 partition of part_unsafe_trigger for values from (0) to (5000);
create table part_unsafe_trigger_2 partition of part_unsafe_trigger for values from (5000) to (10000);
create trigger part_insert_after_trigger_unsafe after insert on part_unsafe_trigger_1
    for each statement execute procedure insert_after_trigger_unsafe();

explain (costs off) insert into part_unsafe_trigger select unique1, stringu1 from tenk1;

--
-- Test that parallel-safety-related changes to partitions are detected and
-- plan cache invalidation is working correctly.
--

create table rp (a int) partition by range (a);
create table rp1 partition of rp for values from (minvalue) to (0);
create table rp2 partition of rp for values from (0) to (maxvalue);
create table foo (a) as select unique1 from tenk1;
prepare q as insert into rp select * from foo where a%2 = 0;
-- should create a parallel plan
explain (costs off) execute q;

create or replace function make_table_bar () returns trigger language
plpgsql as $$ begin create table bar(); return null; end; $$ parallel unsafe;
create trigger ai_rp2 after insert on rp2 for each row execute
function make_table_bar();
-- should create a non-parallel plan
explain (costs off) execute q;

--
-- Test INSERT into table having a DOMAIN column with a CHECK constraint
--
create function sql_is_distinct_from_u(anyelement, anyelement)
returns boolean language sql parallel unsafe
as 'select $1 is distinct from $2 limit 1';

create domain inotnull_u int
  check (sql_is_distinct_from_u(value, null));

create table dom_table_u (x inotnull_u, y int);


-- Test INSERT into table having a DOMAIN column with parallel-unsafe CHECK constraint
explain (costs off) insert into dom_table_u select unique1, unique2 from tenk1;


rollback;

--
-- Clean up anything not created in the transaction
--

drop table names;
drop index names2_fullname_idx;
drop table names2;
drop index names4_fullname_idx;
drop table names4;
drop table testdef;
drop table test_data;

drop function bdefault_unsafe;
drop function cdefault_restricted;
drop function ddefault_safe;
drop function fullname_parallel_unsafe;
drop function fullname_parallel_restricted;
