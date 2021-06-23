# Temporal Range Integrity test
#
# Snapshot integrity fails with simple referential integrity tests,
# but those don't make for good demonstrations because people just
# say that foreign key definitions should be used instead.  There
# are many integrity tests which are conceptually very similar but
# don't have built-in support which will fail when used in triggers.
# This is intended to illustrate such cases.  It is obviously very
# hard to exercise all these permutations when the code is actually
# in a trigger; this test pulls what would normally be inside of
# triggers out to the top level to control the permutations.
#
# Any overlap between the transactions must cause a serialization failure.


setup
{
 CREATE TABLE statute (statute_cite text NOT NULL, eff_date date NOT NULL, exp_date date, CONSTRAINT statute_pkey PRIMARY KEY (statute_cite, eff_date));
 INSERT INTO statute VALUES ('123.45(1)a', DATE '2008-01-01', NULL);
 CREATE TABLE offense (offense_no int NOT NULL, statute_cite text NOT NULL, offense_date date NOT NULL, CONSTRAINT offense_pkey PRIMARY KEY (offense_no));
}

teardown
{
  DROP TABLE statute, offense;
}

session s1
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rx1	{ SELECT count(*) FROM statute WHERE statute_cite = '123.45(1)a' AND eff_date <= DATE '2009-05-15' AND (exp_date IS NULL OR exp_date > DATE '2009-05-15'); }
step wy1	{ INSERT INTO offense VALUES (1, '123.45(1)a', DATE '2009-05-15'); }
step c1		{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step ry2	{ SELECT count(*) FROM offense WHERE statute_cite = '123.45(1)a' AND offense_date >= DATE '2008-01-01'; }
step wx2	{ DELETE FROM statute WHERE statute_cite = '123.45(1)a' AND eff_date = DATE '2008-01-01'; }
step c2		{ COMMIT; }
