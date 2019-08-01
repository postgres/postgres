# Test deadlock resolution with parallel process groups.

# It's fairly hard to get parallel worker processes to block on locks,
# since generally they don't want any locks their leader didn't already
# take.  We cheat like mad here by making a function that takes a lock,
# and is incorrectly marked parallel-safe so that it can execute in a worker.

# Note that we explicitly override any global settings of isolation level
# or force_parallel_mode, to ensure we're testing what we intend to.

# Otherwise, this is morally equivalent to deadlock-soft.spec:
# Four-process deadlock with two hard edges and two soft edges.
# d2 waits for e1 (soft edge), e1 waits for d1 (hard edge),
# d1 waits for e2 (soft edge), e2 waits for d2 (hard edge).
# The deadlock detector resolves the deadlock by reversing the d1-e2 edge,
# unblocking d1.

setup
{
  create function lock_share(int,int) returns int language sql as
  'select pg_advisory_xact_lock_shared($1); select 1;' parallel safe;

  create function lock_excl(int,int) returns int language sql as
  'select pg_advisory_xact_lock($1); select 1;' parallel safe;

  create table bigt as select x from generate_series(1, 10000) x;
  analyze bigt;
}

teardown
{
  drop function lock_share(int,int);
  drop function lock_excl(int,int);
  drop table bigt;
}

session "d1"
setup		{ BEGIN isolation level repeatable read;
			  SET force_parallel_mode = off;
			  SET deadlock_timeout = '10s';
}
# this lock will be taken in the leader, so it will persist:
step "d1a1"	{ SELECT lock_share(1,x) FROM bigt LIMIT 1; }
# this causes all the parallel workers to take locks:
step "d1a2"	{ SET force_parallel_mode = on;
			  SET parallel_setup_cost = 0;
			  SET parallel_tuple_cost = 0;
			  SET min_parallel_table_scan_size = 0;
			  SET parallel_leader_participation = off;
			  SET max_parallel_workers_per_gather = 4;
			  SELECT sum(lock_share(2,x)) FROM bigt; }
step "d1c"	{ COMMIT; }

session "d2"
setup		{ BEGIN isolation level repeatable read;
			  SET force_parallel_mode = off;
			  SET deadlock_timeout = '10ms';
}
# this lock will be taken in the leader, so it will persist:
step "d2a2"	{ select lock_share(2,x) FROM bigt LIMIT 1; }
# this causes all the parallel workers to take locks:
step "d2a1"	{ SET force_parallel_mode = on;
			  SET parallel_setup_cost = 0;
			  SET parallel_tuple_cost = 0;
			  SET min_parallel_table_scan_size = 0;
			  SET parallel_leader_participation = off;
			  SET max_parallel_workers_per_gather = 4;
			  SELECT sum(lock_share(1,x)) FROM bigt; }
step "d2c"	{ COMMIT; }

session "e1"
setup		{ BEGIN isolation level repeatable read;
			  SET force_parallel_mode = on;
			  SET deadlock_timeout = '10s';
}
# this lock will be taken in a parallel worker, but we don't need it to persist
step "e1l"	{ SELECT lock_excl(1,x) FROM bigt LIMIT 1; }
step "e1c"	{ COMMIT; }

session "e2"
setup		{ BEGIN isolation level repeatable read;
			  SET force_parallel_mode = on;
			  SET deadlock_timeout = '10s';
}
# this lock will be taken in a parallel worker, but we don't need it to persist
step "e2l"	{ SELECT lock_excl(2,x) FROM bigt LIMIT 1; }
step "e2c"	{ COMMIT; }

permutation "d1a1" "d2a2" "e1l" "e2l" "d1a2" "d2a1" "d1c" "e1c" "d2c" "e2c"
