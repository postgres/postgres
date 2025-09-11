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

session s2
step s2rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2ins	{ INSERT INTO child VALUES (1, 1); }
step s2c	{ COMMIT; }

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
