# Test deadlock resolution with parallel process groups.

# It's fairly hard to get parallel worker processes to block on locks,
# since generally they don't want any locks their leader didn't already
# take.  We cheat like mad here by creating aliases for advisory-lock
# functions that are incorrectly marked parallel-safe so that they can
# execute in a worker.

# Note that we explicitly override any global settings of isolation level
# or debug_parallel_query, to ensure we're testing what we intend to.

# Otherwise, this is morally equivalent to deadlock-soft.spec:
# Four-process deadlock with two hard edges and two soft edges.
# d2 waits for e1 (soft edge), e1 waits for d1 (hard edge),
# d1 waits for e2 (soft edge), e2 waits for d2 (hard edge).
# The deadlock detector resolves the deadlock by reversing the d1-e2 edge,
# unblocking d1.

# However ... it's not actually that well-defined whether the deadlock
# detector will prefer to unblock d1 or d2.  It depends on which backend
# is first to run DeadLockCheck after the deadlock condition is created:
# that backend will search outwards from its own wait condition, and will
# first find a loop involving the *other* lock.  We encourage that to be
# one of the d2a1 parallel workers, which will therefore unblock d1a2
# workers, by setting a shorter deadlock_timeout in session d2.  But on
# slow machines, one or more d1a2 workers may not yet have reached their
# lock waits, so that they're not unblocked by the first DeadLockCheck.
# The next DeadLockCheck may choose to unblock the d2a1 workers instead,
# which would allow d2a1 to complete before d1a2, causing the test to
# freeze up because isolationtester isn't expecting that completion order.
# (In effect, we have an undetectable deadlock because d2 is waiting for
# d1's completion, but on the client side.)  To fix this, introduce an
# additional lock (advisory lock 3), which is initially taken by d1 and
# then d2a1 will wait for it after completing the main part of the test.
# In this way, the deadlock detector can see that d1 must be completed
# first, regardless of timing.

setup
{
-- The alias functions themselves.  Really these return "void", but
-- the implementation is such that we can declare them to return "int",
-- and we will get a zero result.
  create function lock_share(bigint) returns int language internal as
  'pg_advisory_xact_lock_shared_int8' strict parallel safe;

  create function lock_excl(bigint) returns int language internal as
  'pg_advisory_xact_lock_int8' strict parallel safe;

-- Inline-able wrappers that will produce an integer "1" result:
  create function lock_share(int,int) returns int language sql as
  'select 1 - lock_share($1)' parallel safe;

  create function lock_excl(int,int) returns int language sql as
  'select 1 - lock_excl($1)' parallel safe;

  create table bigt as select x from generate_series(1, 10000) x;
  analyze bigt;
}

teardown
{
  drop function lock_share(int,int);
  drop function lock_excl(int,int);
  drop table bigt;
}

session d1
setup		{ BEGIN isolation level repeatable read;
			  SET debug_parallel_query = off;
			  SET deadlock_timeout = '10s';
}
# these locks will be taken in the leader, so they will persist:
step d1a1	{ SELECT lock_share(1,x), lock_excl(3,x) FROM bigt LIMIT 1; }
# this causes all the parallel workers to take locks:
step d1a2	{ SET debug_parallel_query = on;
			  SET parallel_setup_cost = 0;
			  SET parallel_tuple_cost = 0;
			  SET min_parallel_table_scan_size = 0;
			  SET parallel_leader_participation = off;
			  SET max_parallel_workers_per_gather = 3;
			  SELECT sum(lock_share(2,x)) FROM bigt; }
step d1c	{ COMMIT; }

session d2
setup		{ BEGIN isolation level repeatable read;
			  SET debug_parallel_query = off;
			  SET deadlock_timeout = '10ms';
}
# this lock will be taken in the leader, so it will persist:
step d2a2	{ select lock_share(2,x) FROM bigt LIMIT 1; }
# this causes all the parallel workers to take locks;
# after which, make the leader take lock 3 to prevent client-driven deadlock
step d2a1	{ SET debug_parallel_query = on;
			  SET parallel_setup_cost = 0;
			  SET parallel_tuple_cost = 0;
			  SET min_parallel_table_scan_size = 0;
			  SET parallel_leader_participation = off;
			  SET max_parallel_workers_per_gather = 3;
			  SELECT sum(lock_share(1,x)) FROM bigt;
			  SET debug_parallel_query = off;
			  RESET parallel_setup_cost;
			  RESET parallel_tuple_cost;
			  SELECT lock_share(3,x) FROM bigt LIMIT 1; }
step d2c	{ COMMIT; }

session e1
setup		{ BEGIN isolation level repeatable read;
			  SET debug_parallel_query = on;
			  SET deadlock_timeout = '10s';
}
# this lock will be taken in a parallel worker, but we don't need it to persist
step e1l	{ SELECT lock_excl(1,x) FROM bigt LIMIT 1; }
step e1c	{ COMMIT; }

session e2
setup		{ BEGIN isolation level repeatable read;
			  SET debug_parallel_query = on;
			  SET deadlock_timeout = '10s';
}
# this lock will be taken in a parallel worker, but we don't need it to persist
step e2l	{ SELECT lock_excl(2,x) FROM bigt LIMIT 1; }
step e2c	{ COMMIT; }

permutation d1a1 d2a2 e1l e2l d1a2 d2a1 d1c e1c d2c e2c
