# RI Trigger test
#
# Test C-based temporal referential integrity enforcement.
# Under REPEATABLE READ we need some snapshot trickery in C,
# or we would permit things that violate referential integrity.

setup
{
  CREATE TABLE parent (
	id int4range NOT NULL,
	valid_at daterange NOT NULL,
	PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));
  CREATE TABLE child (
	id int4range NOT NULL,
	valid_at daterange NOT NULL,
	parent_id int4range,
	FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES parent);
  INSERT INTO parent VALUES ('[1,2)', '[2020-01-01,2030-01-01)');
}

teardown { DROP TABLE parent, child; }

session s1
step s1rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1del	{ DELETE FROM parent WHERE id = '[1,2)'; }
step s1upok	{ UPDATE parent SET valid_at = '[2020-01-01,2026-01-01)' WHERE id = '[1,2)'; }
step s1upbad	{ UPDATE parent SET valid_at = '[2020-01-01,2024-01-01)' WHERE id = '[1,2)'; }
step s1c	{ COMMIT; }

session s2
step s2rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2ins	{
  INSERT INTO child VALUES ('[1,2)', '[2020-01-01,2025-01-01)', '[1,2)');
}
step s2c	{ COMMIT; }

# Violates referential integrity unless we use an up-to-date crosscheck snapshot:
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

# Also check updating the valid time (without violating RI):

# ...with READ COMMITTED:
permutation s1rc s2rc s2ins s1upok s2c s1c
permutation s1rc s2rc s1upok s2ins s1c s2c
# ...with REPEATABLE READ:
permutation s1rr s2rr s2ins s1upok s2c s1c
permutation s1rr s2rr s1upok s2ins s1c s2c
# ...with SERIALIZABLE:
permutation s1ser s2ser s2ins s1upok s2c s1c
permutation s1ser s2ser s1upok s2ins s1c s2c

# Also check updating the valid time (while violating RI):

# ...with READ COMMITTED:
permutation s1rc s2rc s2ins s1upbad s2c s1c
permutation s1rc s2rc s1upbad s2ins s1c s2c
# ...with REPEATABLE READ:
permutation s1rr s2rr s2ins s1upbad s2c s1c
permutation s1rr s2rr s1upbad s2ins s1c s2c
# ...with SERIALIZABLE:
permutation s1ser s2ser s2ins s1upbad s2c s1c
permutation s1ser s2ser s1upbad s2ins s1c s2c
