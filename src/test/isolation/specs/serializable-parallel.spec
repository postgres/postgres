# The example from the paper "A read-only transaction anomaly under snapshot
# isolation"[1].
#
# Here we test that serializable snapshot isolation (SERIALIZABLE) doesn't
# suffer from the anomaly, because s2 is aborted upon detection of a cycle.
# In this case the read only query s3 happens to be running in a parallel
# worker.
#
# [1] http://www.cs.umb.edu/~poneil/ROAnom.pdf

setup
{
	CREATE TABLE bank_account (id TEXT PRIMARY KEY, balance DECIMAL NOT NULL);
	INSERT INTO bank_account (id, balance) VALUES ('X', 0), ('Y', 0);
}

teardown
{
	DROP TABLE bank_account;
}

session "s1"
setup 		{ BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; }
step "s1ry"	{ SELECT balance FROM bank_account WHERE id = 'Y'; }
step "s1wy"	{ UPDATE bank_account SET balance = 20 WHERE id = 'Y'; }
step "s1c" 	{ COMMIT; }

session "s2"
setup		{ BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; }
step "s2rx"	{ SELECT balance FROM bank_account WHERE id = 'X'; }
step "s2ry"	{ SELECT balance FROM bank_account WHERE id = 'Y'; }
step "s2wx"	{ UPDATE bank_account SET balance = -11 WHERE id = 'X'; }
step "s2c"	{ COMMIT; }

session "s3"
setup		{
			  BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
			  SET force_parallel_mode = on;
			}
step "s3r"	{ SELECT id, balance FROM bank_account WHERE id IN ('X', 'Y') ORDER BY id; }
step "s3c"	{ COMMIT; }

# without s3, s1 and s2 commit
permutation "s2rx" "s2ry" "s1ry" "s1wy" "s1c" "s2wx" "s2c" "s3c"

# once s3 observes the data committed by s1, a cycle is created and s2 aborts
permutation "s2rx" "s2ry" "s1ry" "s1wy" "s1c" "s3r" "s3c" "s2wx"
