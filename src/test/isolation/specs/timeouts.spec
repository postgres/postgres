# Simple tests for statement_timeout and lock_timeout features

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
