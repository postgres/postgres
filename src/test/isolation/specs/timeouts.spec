# Simple tests for statement_timeout, lock_timeout and transaction_timeout features

setup
{
 CREATE TABLE accounts (accountid text PRIMARY KEY, balance numeric not null);
 INSERT INTO accounts VALUES ('checking', 600), ('savings', 600);
}

teardown
{
 DROP TABLE accounts;
}

session s1
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step rdtbl	{ SELECT * FROM accounts; }
step wrtbl	{ UPDATE accounts SET balance = balance + 100; }
teardown	{ ABORT; }

session s2
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step sto	{ SET statement_timeout = '10ms'; }
step lto	{ SET lock_timeout = '10ms'; }
step lsto	{ SET lock_timeout = '10ms'; SET statement_timeout = '10s'; }
step slto	{ SET lock_timeout = '10s'; SET statement_timeout = '10ms'; }
step locktbl	{ LOCK TABLE accounts; }
step update	{ DELETE FROM accounts WHERE accountid = 'checking'; }
teardown	{ ABORT; }

session s3
step s3_begin	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step stto	{ SET statement_timeout = '10ms'; SET transaction_timeout = '1s'; }
step tsto	{ SET statement_timeout = '1s'; SET transaction_timeout = '10ms'; }
step s3_sleep	{ SELECT pg_sleep(0.1); }
step s3_abort	{ ABORT; }

session s4
step s4_begin	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step itto	{ SET idle_in_transaction_session_timeout = '10ms'; SET transaction_timeout = '1s'; }

session s5
step s5_begin	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step tito	{ SET idle_in_transaction_session_timeout = '1s'; SET transaction_timeout = '10ms'; }

session s6
step s6_begin	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s6_tt	{ SET statement_timeout = '1s'; SET transaction_timeout = '10ms'; }

session checker
step checker_sleep	{ SELECT pg_sleep(0.1); }
step s3_check	{ SELECT count(*) FROM pg_stat_activity WHERE application_name = 'isolation/timeouts/s3'; }
step s4_check	{ SELECT count(*) FROM pg_stat_activity WHERE application_name = 'isolation/timeouts/s4'; }
step s5_check	{ SELECT count(*) FROM pg_stat_activity WHERE application_name = 'isolation/timeouts/s5'; }
step s6_check	{ SELECT count(*) FROM pg_stat_activity WHERE application_name = 'isolation/timeouts/s6'; }


# It's possible that the isolation tester will not observe the final
# steps as "waiting", thanks to the relatively short timeouts we use.
# We can ensure consistent test output by marking those steps with (*).

# statement timeout, table-level lock
permutation rdtbl sto locktbl(*)
# lock timeout, table-level lock
permutation rdtbl lto locktbl(*)
# lock timeout expires first, table-level lock
permutation rdtbl lsto locktbl(*)
# statement timeout expires first, table-level lock
permutation rdtbl slto locktbl(*)
# statement timeout, row-level lock
permutation wrtbl sto update(*)
# lock timeout, row-level lock
permutation wrtbl lto update(*)
# lock timeout expires first, row-level lock
permutation wrtbl lsto update(*)
# statement timeout expires first, row-level lock
permutation wrtbl slto update(*)

# statement timeout expires first
permutation stto s3_begin s3_sleep s3_check s3_abort
# transaction timeout expires first, session s3 FATAL-out
permutation tsto s3_begin checker_sleep s3_check
# idle in transaction timeout expires first, session s4 FATAL-out
permutation itto s4_begin checker_sleep s4_check
# transaction timeout expires first, session s5 FATAL-out
permutation tito s5_begin checker_sleep s5_check
# transaction timeout can be schedule amid transaction, session s6 FATAL-out
permutation s6_begin s6_tt checker_sleep s6_check