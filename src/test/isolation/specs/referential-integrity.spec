# Referential Integrity test
#
# The assumption here is that the application code issuing the SELECT
# to test for the presence or absence of a related record would do the
# right thing -- this script doesn't include that logic.
#
# Any overlap between the transactions must cause a serialization failure.

setup
{
 CREATE TABLE a (i int PRIMARY KEY);
 CREATE TABLE b (a_id int);
 INSERT INTO a VALUES (1);
}

teardown
{
 DROP TABLE a, b;
}

session "s1"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "rx1"	{ SELECT i FROM a WHERE i = 1; }
step "wy1"	{ INSERT INTO b VALUES (1); }
step "c1"	{ COMMIT; }

session "s2"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "rx2"	{ SELECT i FROM a WHERE i = 1; }
step "ry2"	{ SELECT a_id FROM b WHERE a_id = 1; }
step "wx2"	{ DELETE FROM a WHERE i = 1; }
step "c2"	{ COMMIT; }
