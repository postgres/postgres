# Tests for the EvalPlanQual mechanism
#
# EvalPlanQual is used in READ COMMITTED isolation level to attempt to
# re-execute UPDATE and DELETE operations against rows that were updated
# by some concurrent transaction.

setup
{
 CREATE TABLE accounts (accountid text PRIMARY KEY, balance numeric not null);
 INSERT INTO accounts VALUES ('checking', 600), ('savings', 600);

 CREATE TABLE p (a int, b int, c int);
 CREATE TABLE c1 () INHERITS (p);
 CREATE TABLE c2 () INHERITS (p);
 CREATE TABLE c3 () INHERITS (p);
 INSERT INTO c1 SELECT 0, a / 3, a % 3 FROM generate_series(0, 9) a;
 INSERT INTO c2 SELECT 1, a / 3, a % 3 FROM generate_series(0, 9) a;
 INSERT INTO c3 SELECT 2, a / 3, a % 3 FROM generate_series(0, 9) a;
}

teardown
{
 DROP TABLE accounts;
 DROP TABLE p CASCADE;
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

# tests with table p check inheritance cases:
# readp1/writep1/readp2 tests a bug where nodeLockRows did the wrong thing
# when the first updated tuple was in a non-first child table.
# writep2/returningp1 tests a memory allocation issue

step "readp1"	{ SELECT tableoid::regclass, ctid, * FROM p WHERE b IN (0, 1) AND c = 0 FOR UPDATE; }
step "writep1"	{ UPDATE p SET b = -1 WHERE a = 1 AND b = 1 AND c = 0; }
step "writep2"	{ UPDATE p SET b = -b WHERE a = 1 AND c = 0; }
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
step "readp2"	{ SELECT tableoid::regclass, ctid, * FROM p WHERE b IN (0, 1) AND c = 0 FOR UPDATE; }
step "returningp1" {
	WITH u AS ( UPDATE p SET b = b WHERE a > 0 RETURNING * )
	  SELECT * FROM u;
}
step "c2"	{ COMMIT; }

session "s3"
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step "read"	{ SELECT * FROM accounts ORDER BY accountid; }
teardown	{ COMMIT; }

permutation "wx1" "wx2" "c1" "c2" "read"
permutation "wy1" "wy2" "c1" "c2" "read"
permutation "upsert1" "upsert2" "c1" "c2" "read"
permutation "readp1" "writep1" "readp2" "c1" "c2"
permutation "writep2" "returningp1" "c1" "c2"
