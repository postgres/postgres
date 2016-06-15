--
-- PARALLEL
--

-- Serializable isolation would disable parallel query, so explicitly use an
-- arbitrary other level.
begin isolation level repeatable read;

-- setup parallel test
set parallel_setup_cost=0;
set parallel_tuple_cost=0;

explain (costs off)
  select count(*) from a_star;
select count(*) from a_star;

set force_parallel_mode=1;

explain (costs off)
  select stringu1::int2 from tenk1 where unique1 = 1;

do $$begin
  -- Provoke error in worker.  The original message CONTEXT contains a worker
  -- PID that must be hidden in the test output.
  perform stringu1::int2 from tenk1 where unique1 = 1;
  exception
	when others then
		raise 'SQLERRM: %', sqlerrm;
end$$;

rollback;
