--
-- EXPLAIN
--
-- There are many test cases elsewhere that use EXPLAIN as a vehicle for
-- checking something else (usually planner behavior).  This file is
-- concerned with testing EXPLAIN in its own right.
--

-- To produce stable regression test output, it's usually necessary to
-- ignore details such as exact costs or row counts.  These filter
-- functions replace changeable output details with fixed strings.

create function explain_filter(text) returns setof text
language plpgsql as
$$
declare
    ln text;
begin
    for ln in execute $1
    loop
        -- Replace any numeric word with just 'N'
        ln := regexp_replace(ln, '-?\m\d+\M', 'N', 'g');
        -- In sort output, the above won't match units-suffixed numbers
        ln := regexp_replace(ln, '\m\d+kB', 'NkB', 'g');
        -- Ignore text-mode buffers output because it varies depending
        -- on the system state
        CONTINUE WHEN (ln ~ ' +Buffers: .*');
        -- Ignore text-mode "Planning:" line because whether it's output
        -- varies depending on the system state
        CONTINUE WHEN (ln = 'Planning:');
        return next ln;
    end loop;
end;
$$;

-- To produce valid JSON output, replace numbers with "0" or "0.0" not "N"
create function explain_filter_to_json(text) returns jsonb
language plpgsql as
$$
declare
    data text := '';
    ln text;
begin
    for ln in execute $1
    loop
        -- Replace any numeric word with just '0'
        ln := regexp_replace(ln, '\m\d+\M', '0', 'g');
        data := data || ln;
    end loop;
    return data::jsonb;
end;
$$;

-- Disable JIT, or we'll get different output on machines where that's been
-- forced on
set jit = off;

-- Similarly, disable track_io_timing, to avoid output differences when
-- enabled.
set track_io_timing = off;

-- Simple cases

select explain_filter('explain select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers off) select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers off, verbose) select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers, format text) select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers, format xml) select * from int8_tbl i8');
select explain_filter('explain (analyze, serialize, buffers, format yaml) select * from int8_tbl i8');
select explain_filter('explain (buffers, format text) select * from int8_tbl i8');
select explain_filter('explain (buffers, format json) select * from int8_tbl i8');

-- Check expansion of window definitions

select explain_filter('explain verbose select sum(unique1) over w, sum(unique2) over (w order by hundred), sum(tenthous) over (w order by hundred) from tenk1 window w as (partition by ten)');
select explain_filter('explain verbose select sum(unique1) over w1, sum(unique2) over (w1 order by hundred), sum(tenthous) over (w1 order by hundred rows 10 preceding) from tenk1 window w1 as (partition by ten)');

-- Check output including I/O timings.  These fields are conditional
-- but always set in JSON format, so check them only in this case.
set track_io_timing = on;
select explain_filter('explain (analyze, buffers, format json) select * from int8_tbl i8');
set track_io_timing = off;

-- SETTINGS option
-- We have to ignore other settings that might be imposed by the environment,
-- so printing the whole Settings field unfortunately won't do.

begin;
set local plan_cache_mode = force_generic_plan;
select true as "OK"
  from explain_filter('explain (settings) select * from int8_tbl i8') ln
  where ln ~ '^ *Settings: .*plan_cache_mode = ''force_generic_plan''';
select explain_filter_to_json('explain (settings, format json) select * from int8_tbl i8') #> '{0,Settings,plan_cache_mode}';
rollback;

-- GENERIC_PLAN option

select explain_filter('explain (generic_plan) select unique1 from tenk1 where thousand = $1');
-- should fail
select explain_filter('explain (analyze, generic_plan) select unique1 from tenk1 where thousand = $1');

-- MEMORY option
select explain_filter('explain (memory) select * from int8_tbl i8');
select explain_filter('explain (memory, analyze, buffers off) select * from int8_tbl i8');
select explain_filter('explain (memory, summary, format yaml) select * from int8_tbl i8');
select explain_filter('explain (memory, analyze, format json) select * from int8_tbl i8');
prepare int8_query as select * from int8_tbl i8;
select explain_filter('explain (memory) execute int8_query');

-- Test EXPLAIN (GENERIC_PLAN) with partition pruning
-- partitions should be pruned at plan time, based on constants,
-- but there should be no pruning based on parameter placeholders
create table gen_part (
  key1 integer not null,
  key2 integer not null
) partition by list (key1);
create table gen_part_1
  partition of gen_part for values in (1)
  partition by range (key2);
create table gen_part_1_1
  partition of gen_part_1 for values from (1) to (2);
create table gen_part_1_2
  partition of gen_part_1 for values from (2) to (3);
create table gen_part_2
  partition of gen_part for values in (2);
-- should scan gen_part_1_1 and gen_part_1_2, but not gen_part_2
select explain_filter('explain (generic_plan) select key1, key2 from gen_part where key1 = 1 and key2 = $1');
drop table gen_part;

--
-- Test production of per-worker data
--
-- Unfortunately, because we don't know how many worker processes we'll
-- actually get (maybe none at all), we can't examine the "Workers" output
-- in any detail.  We can check that it parses correctly as JSON, and then
-- remove it from the displayed results.

begin;
-- encourage use of parallel plans
set parallel_setup_cost=0;
set parallel_tuple_cost=0;
set min_parallel_table_scan_size=0;
set max_parallel_workers_per_gather=4;

select jsonb_pretty(
  explain_filter_to_json('explain (analyze, verbose, buffers, format json)
                         select * from tenk1 order by tenthous')
  -- remove "Workers" node of the Seq Scan plan node
  #- '{0,Plan,Plans,0,Plans,0,Workers}'
  -- remove "Workers" node of the Sort plan node
  #- '{0,Plan,Plans,0,Workers}'
  -- Also remove its sort-type fields, as those aren't 100% stable
  #- '{0,Plan,Plans,0,Sort Method}'
  #- '{0,Plan,Plans,0,Sort Space Type}'
);

rollback;

-- Test display of temporary objects
create temp table t1(f1 float8);

create function pg_temp.mysin(float8) returns float8 language plpgsql
as 'begin return sin($1); end';

select explain_filter('explain (verbose) select * from t1 where pg_temp.mysin(f1) < 0.5');

-- Test compute_query_id
set compute_query_id = on;
select explain_filter('explain (verbose) select * from int8_tbl i8');

-- Test compute_query_id with utility statements containing plannable query
select explain_filter('explain (verbose) declare test_cur cursor for select * from int8_tbl');
select explain_filter('explain (verbose) create table test_ctas as select 1');

-- Test SERIALIZE option
select explain_filter('explain (analyze,buffers off,serialize) select * from int8_tbl i8');
select explain_filter('explain (analyze,serialize text,buffers,timing off) select * from int8_tbl i8');
select explain_filter('explain (analyze,serialize binary,buffers,timing) select * from int8_tbl i8');
-- this tests an edge case where we have no data to return
select explain_filter('explain (analyze,buffers off,serialize) create temp table explain_temp as select * from int8_tbl i8');

-- Test tuplestore storage usage in Window aggregate (memory case)
select explain_filter('explain (analyze,buffers off,costs off) select sum(n) over() from generate_series(1,10) a(n)');
-- Test tuplestore storage usage in Window aggregate (disk case)
set work_mem to 64;
select explain_filter('explain (analyze,buffers off,costs off) select sum(n) over() from generate_series(1,2000) a(n)');
-- Test tuplestore storage usage in Window aggregate (memory and disk case, final result is disk)
select explain_filter('explain (analyze,buffers off,costs off) select sum(n) over(partition by m) from (SELECT n < 3 as m, n from generate_series(1,2000) a(n))');
reset work_mem;
