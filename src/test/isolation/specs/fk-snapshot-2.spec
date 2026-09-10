# RI Trigger test
#
# Test C-based referential integrity enforcement.
# Under REPEATABLE READ we need some snapshot trickery in C,
# or we would permit things that violate referential integrity.

setup
{
  CREATE TABLE parent (parent_id SERIAL NOT NULL PRIMARY KEY);
  CREATE TABLE child (
	child_id SERIAL NOT NULL PRIMARY KEY,
	parent_id INTEGER REFERENCES parent);
  INSERT INTO parent VALUES(1);
}

teardown { DROP TABLE parent, child; }

session s1
step s1rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1del	{ DELETE FROM parent WHERE parent_id = 1; }
step s1c	{ COMMIT; }
step s1ins	{ INSERT INTO parent VALUES (2); }

session s2
step s2rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2ins	{ INSERT INTO child VALUES (1, 1); }
step s2c	{ COMMIT; }
step s2sel	{ SELECT count(*) FROM parent; }
step s2ins2	{ INSERT INTO child VALUES (2, 2); }

# Violates referential integrity unless we use a crosscheck snapshot,
# which is up-to-date compared with the transaction's snapshot.
permutation s1rr s2rr s2ins s1del s2c s1c

# Raises a can't-serialize exception
# when the INSERT trigger does SELECT FOR KEY SHARE:
permutation s1rr s2rr s1del s2ins s1c s2c

# Test the same scenarios in READ COMMITTED:
# A crosscheck snapshot is not required here.
permutation s1rc s2rc s2ins s1del s2c s1c
permutation s1rc s2rc s1del s2ins s1c s2c

# Test the same scenarios in SERIALIZABLE:
# We should report the FK violation:
permutation s1ser s2ser s2ins s1del s2c s1c
# We raise a concurrent update error
# which is good enough:
permutation s1ser s2ser s1del s2ins s1c s2c

# A parent row committed by another transaction after this one took its
# snapshot.  RI_FKey_check passes detectNewRows = false, so the check runs
# under the transaction snapshot rather than a current one, and the row is
# correctly not visible: referencing it is a violation.  Only the parent-side
# checks need to see rows committed since the snapshot.
permutation s1rr s2rr s2sel s1ins s1c s2ins2 s2c
permutation s1ser s2ser s2sel s1ins s1c s2ins2 s2c

# The same order in READ COMMITTED, where the check's snapshot is a fresh one
# and the parent row is visible.
permutation s1rc s2rc s2sel s1ins s1c s2ins2 s2c
