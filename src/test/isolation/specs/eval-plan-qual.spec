# Tests for the EvalPlanQual mechanism
#
# EvalPlanQual is used in READ COMMITTED isolation level to attempt to
# re-execute UPDATE and DELETE operations against rows that were updated
# by some concurrent transaction.

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
# wx1 then wx2 checks the basic case of re-fetching up-to-date values
step "wx1"	{ UPDATE accounts SET balance = balance - 200 WHERE accountid = 'checking'; }
# wy1 then wy2 checks the case where quals pass then fail
step "wy1"	{ UPDATE accounts SET balance = balance + 500 WHERE accountid = 'checking'; }
# upsert tests are to check writable-CTE cases
step "upsert1"	{
	WITH upsert AS
	  (UPDATE accounts SET balance = balance + 500
	   WHERE accountid = 'savings'
	   RETURNING accountid)
	INSERT INTO accounts SELECT 'savings', 500
	  WHERE NOT EXISTS (SELECT 1 FROM upsert);
}
step "c1"	{ COMMIT; }

session "s2"
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step "wx2"	{ UPDATE accounts SET balance = balance + 450 WHERE accountid = 'checking'; }
step "wy2"	{ UPDATE accounts SET balance = balance + 1000 WHERE accountid = 'checking' AND balance < 1000; }
step "upsert2"	{
	WITH upsert AS
	  (UPDATE accounts SET balance = balance + 1234
	   WHERE accountid = 'savings'
	   RETURNING accountid)
	INSERT INTO accounts SELECT 'savings', 1234
	  WHERE NOT EXISTS (SELECT 1 FROM upsert);
}
step "c2"	{ COMMIT; }

session "s3"
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step "read"	{ SELECT * FROM accounts ORDER BY accountid; }
teardown	{ COMMIT; }

permutation "wx1" "wx2" "c1" "c2" "read"
permutation "wy1" "wy2" "c1" "c2" "read"
permutation "upsert1" "upsert2" "c1" "c2" "read"
