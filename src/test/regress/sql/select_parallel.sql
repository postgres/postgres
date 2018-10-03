--
-- PARALLEL
--

create or replace function parallel_restricted(int) returns int as
  $$begin return $1; end$$ language plpgsql parallel restricted;

-- Serializable isolation would disable parallel query, so explicitly use an
-- arbitrary other level.
begin isolation level repeatable read;

-- encourage use of parallel plans
set parallel_setup_cost=0;
set parallel_tuple_cost=0;
set min_parallel_relation_size=0;
set max_parallel_workers_per_gather=4;

explain (costs off)
  select count(*) from a_star;
select count(*) from a_star;

-- test that parallel_restricted function doesn't run in worker
alter table tenk1 set (parallel_workers = 4);
explain (verbose, costs off)
select parallel_restricted(unique1) from tenk1
  where stringu1 = 'GRAAAA' order by 1;

-- test parallel plan when group by expression is in target list.
explain (costs off)
	select length(stringu1) from tenk1 group by length(stringu1);
select length(stringu1) from tenk1 group by length(stringu1);

explain (costs off)
	select stringu1, count(*) from tenk1 group by stringu1 order by stringu1;

-- test that parallel plan for aggregates is not selected when
-- target list contains parallel restricted clause.
explain (costs off)
	select  sum(parallel_restricted(unique1)) from tenk1
	group by(parallel_restricted(unique1));

-- check parallelized int8 aggregate (bug #14897)
explain (costs off)
select avg(aa::int8) from a_star;

select avg(aa::int8) from a_star;

-- test accumulation of stats for parallel nodes
set enable_indexscan to off;
set enable_bitmapscan to off;
set enable_material to off;
alter table tenk2 set (parallel_workers = 0);
create function explain_parallel_stats() returns setof text
language plpgsql as
$$
declare ln text;
begin
    for ln in
        explain (analyze, timing off, costs off)
          select count(*) from tenk1, tenk2 where
            tenk1.hundred > 1 and tenk2.thousand=0
    loop
        ln := regexp_replace(ln, 'Planning time: \S*',  'Planning time: xxx');
        ln := regexp_replace(ln, 'Execution time: \S*', 'Execution time: xxx');
        return next ln;
    end loop;
end;
$$;
select * from explain_parallel_stats();
reset enable_indexscan;
reset enable_bitmapscan;
reset enable_material;
alter table tenk2 reset (parallel_workers);
drop function explain_parallel_stats();

-- test the sanity of parallel query after the active role is dropped.
set force_parallel_mode=1;
drop role if exists regress_parallel_worker;
create role regress_parallel_worker;
set role regress_parallel_worker;
reset session authorization;
drop role regress_parallel_worker;
select count(*) from tenk1;
reset role;

-- Window function calculation can't be pushed to workers.
explain (costs off, verbose)
  select count(*) from tenk1 a where (unique1, two) in
    (select unique1, row_number() over() from tenk1 b);

-- LIMIT/OFFSET within sub-selects can't be pushed to workers.
explain (costs off)
  select * from tenk1 a where two in
    (select two from tenk1 b where stringu1 like '%AAAA' limit 3);

explain (costs off)
  select stringu1::int2 from tenk1 where unique1 = 1;

-- test passing expanded-value representations to workers
CREATE FUNCTION make_some_array(int,int) returns int[] as
$$declare x int[];
  begin
    x[1] := $1;
    x[2] := $2;
    return x;
  end$$ language plpgsql parallel safe;
CREATE TABLE fooarr(f1 text, f2 int[], f3 text);
INSERT INTO fooarr VALUES('1', ARRAY[1,2], 'one');

PREPARE pstmt(text, int[]) AS SELECT * FROM fooarr WHERE f1 = $1 AND f2 = $2;
EXPLAIN (COSTS OFF) EXECUTE pstmt('1', make_some_array(1,2));
EXECUTE pstmt('1', make_some_array(1,2));
DEALLOCATE pstmt;

do $$begin
  -- Provoke error, possibly in worker.  If this error happens to occur in
  -- the worker, there will be a CONTEXT line which must be hidden.
  perform stringu1::int2 from tenk1 where unique1 = 1;
  exception
	when others then
		raise 'SQLERRM: %', sqlerrm;
end$$;

rollback;
