# Total Cash test
#
# Another famous test of snapshot isolation anomaly.
#
# Any overlap between the transactions must cause a serialization failure.

setup
{
 CREATE TABLE accounts (accountid text NOT NULL PRIMARY KEY, balance numeric not null);
 INSERT INTO accounts VALUES ('checking', 600),('savings',600);
}

teardown
{
 DROP TABLE accounts;
}

session "s1"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "wx1"	{ UPDATE accounts SET balance = balance - 200 WHERE accountid = 'checking'; }
step "rxy1"	{ SELECT SUM(balance) FROM accounts; }
step "c1"	{ COMMIT; }

session "s2"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "wy2"	{ UPDATE accounts SET balance = balance - 200 WHERE accountid = 'savings'; }
step "rxy2"	{ SELECT SUM(balance) FROM accounts; }
step "c2"	{ COMMIT; }
