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

session "s1"
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step "rdtbl"	{ SELECT * FROM accounts; }
step "wrtbl"	{ UPDATE accounts SET balance = balance + 100; }
teardown	{ ABORT; }

session "s2"
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step "sto"	{ SET statement_timeout = 5000; }
step "lto"	{ SET lock_timeout = 5000; }
step "lsto"	{ SET lock_timeout = 5000; SET statement_timeout = 6000; }
step "slto"	{ SET lock_timeout = 6000; SET statement_timeout = 5000; }
step "locktbl"	{ LOCK TABLE accounts; }
step "update"	{ DELETE FROM accounts WHERE accountid = 'checking'; }
teardown	{ ABORT; }

# statement timeout, table-level lock
permutation "rdtbl" "sto" "locktbl"
# lock timeout, table-level lock
permutation "rdtbl" "lto" "locktbl"
# lock timeout expires first, table-level lock
permutation "rdtbl" "lsto" "locktbl"
# statement timeout expires first, table-level lock
permutation "rdtbl" "slto" "locktbl"
# statement timeout, row-level lock
permutation "wrtbl" "sto" "update"
# lock timeout, row-level lock
permutation "wrtbl" "lto" "update"
# lock timeout expires first, row-level lock
permutation "wrtbl" "lsto" "update"
# statement timeout expires first, row-level lock
permutation "wrtbl" "slto" "update"
