# UPDATE/DELETE FOR PORTION OF test
#
# Test inserting temporal leftovers from a FOR PORTION OF update/delete.
#
# In READ COMMITTED mode, concurrent updates/deletes to the same records cause
# weird results. Portions of history that should have been updated/deleted don't
# get changed. That's because the leftovers from one operation are added too
# late to be seen by the other. EvalPlanQual will reload the changed-in-common
# row, but it won't re-scan to find new leftovers.
#
# MariaDB similarly gives undesirable results in READ COMMITTED mode (although
# not the same results). DB2 doesn't have READ COMMITTED, but it gives correct
# results at all levels, in particular READ STABILITY (which seems closest).
#
# A workaround is to lock the part of history you want before changing it (using
# SELECT FOR UPDATE). That way the search for rows is late enough to see
# leftovers from the other session(s). This shouldn't impose any new deadlock
# risks, since the locks are the same as before. Adding a third/fourth/etc.
# connection also doesn't change the semantics. The READ COMMITTED tests here
# demonstrate the problem and also show that solving it with manual locks is
# viable and not vitiated by any bugs. Incidentally, this approach also works in
# MariaDB.
#
# We run the same tests under REPEATABLE READ to show the problem goes away.
# In general they do what you'd want with no explicit locking required, but some
# orderings raise a concurrent update/delete failure (as expected). If there is
# a prior read by s1, concurrent update/delete failures are more common.
#
# To save on test time, we only run a couple SERIALIZABLE tests (for the more
# problematic permutations).
#
# We test updates where s2 updates history that is:
#
# - non-overlapping with s1,
# - contained entirely in s1,
# - partly contained in s1.
#
# We don't need to test where s2 entirely contains s1 because of symmetry:
# we test both when s1 precedes s2 and when s2 precedes s1,  so that scenario is
# covered.
#
# We test various orderings of the update/delete/commit from s1 and s2.
# Note that `s1lock s2lock s1change` is boring because it's the same as
# `s1lock s1change s2lock`. In other words it doesn't matter if something
# interposes between the lock and its change (as long as everyone is following
# the same policy).

setup
{
  CREATE TABLE products (
	id int4range NOT NULL,
	valid_at daterange NOT NULL,
	price decimal NOT NULL,
	PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));
  INSERT INTO products VALUES
	('[1,2)', '[2020-01-01,2030-01-01)', 5.00);
}

teardown { DROP TABLE products; }

session s1
setup		{ SET datestyle TO ISO, YMD; }
step s1rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1lock2025 {
  SELECT * FROM products
  WHERE id = '[1,2)' AND valid_at && '[2025-01-01,2026-01-01)'
  ORDER BY valid_at FOR UPDATE;
}
step s1upd2025	{
  UPDATE products
  FOR PORTION OF valid_at FROM '2025-01-01' TO '2026-01-01'
  SET price = 8.00
  WHERE id = '[1,2)';
}
step s1del2025 {
  DELETE FROM products
  FOR PORTION OF valid_at FROM '2025-01-01' TO '2026-01-01'
  WHERE id = '[1,2)';
}
step s1q	{ SELECT * FROM products ORDER BY id, valid_at; }
step s1c	{ COMMIT; }

session s2
setup		{ SET datestyle TO ISO, YMD; }
step s2rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2lock202503 {
  SELECT * FROM products
  WHERE id = '[1,2)' AND valid_at && '[2025-03-01,2025-04-01)'
  ORDER BY valid_at FOR UPDATE;
}
step s2lock20252026 {
  SELECT * FROM products
  WHERE id = '[1,2)' AND valid_at && '[2025-06-01,2026-06-01)'
  ORDER BY valid_at FOR UPDATE;
}
step s2lock2027 {
  SELECT * FROM products
  WHERE id = '[1,2)' AND valid_at && '[2027-01-01,2028-01-01)'
  ORDER BY valid_at FOR UPDATE;
}
step s2upd202503 {
  UPDATE products
  FOR PORTION OF valid_at FROM '2025-03-01' TO '2025-04-01'
  SET price = 10.00
  WHERE id = '[1,2)';
}
step s2upd20252026 {
  UPDATE products
  FOR PORTION OF valid_at FROM '2025-06-01' TO '2026-06-01'
  SET price = 10.00
  WHERE id = '[1,2)';
}
step s2upd2027 {
  UPDATE products
  FOR PORTION OF valid_at FROM '2027-01-01' TO '2028-01-01'
  SET price = 10.00
  WHERE id = '[1,2)';
}
step s2del202503 {
  DELETE FROM products
  FOR PORTION OF valid_at FROM '2025-03-01' TO '2025-04-01'
  WHERE id = '[1,2)';
}
step s2del20252026 {
  DELETE FROM products
  FOR PORTION OF valid_at FROM '2025-06-01' TO '2026-06-01'
  WHERE id = '[1,2)';
}
step s2del2027 {
  DELETE FROM products
  FOR PORTION OF valid_at FROM '2027-01-01' TO '2028-01-01'
  WHERE id = '[1,2)';
}
step s2c	{ COMMIT; }

# ########################################
# READ COMMITTED tests, UPDATE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1rc s2rc s2lock2027 s2upd2027 s2c s1lock2025 s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rc s2rc s2lock202503 s2upd202503 s2c s1lock2025 s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rc s2rc s2lock20252026 s2upd20252026 s2c s1lock2025 s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock2027 s2upd2027 s2c s1q

# s2 loads the updated row
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock202503 s2upd202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock20252026 s2upd20252026 s2c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be updated either.
permutation s1rc s2rc s2upd2027 s1upd2025 s2c s1c s1q

# Workaround:
# s1 updates the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock2027 s2upd2027 s1lock2025 s2c s1upd2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# but EvalPlanQual still matches the row to be updated.
permutation s1rc s2rc s2upd202503 s1upd2025 s2c s1c s1q

# Workaround:
# s1 overwrites the row from s2 and sees its leftovers
permutation s1rc s2rc s2lock202503 s2upd202503 s1lock2025 s2c s1upd2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# but EvalPlanQual still matches the row to be updated,
# and s1's leftovers don't conflict with s2's.
permutation s1rc s2rc s2upd20252026 s1upd2025 s2c s1c s1q

# Workaround:
# s1 overwrites the row from s2 and sees its leftovers
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock20252026 s2upd20252026 s1lock2025 s2c s1upd2025 s1c s1q

# ########################################
# READ COMMITTED tests, UPDATE+DELETE:
# ########################################

# s1 sees the leftovers
permutation s1rc s2rc s2lock2027 s2del2027 s2c s1lock2025 s1upd2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s2lock202503 s2del202503 s2c s1lock2025 s1upd2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s2lock20252026 s2del20252026 s2c s1lock2025 s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock2027 s2del2027 s2c s1q

# s2 loads the updated row
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock202503 s2del202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock20252026 s2del20252026 s2c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be updated either.
permutation s1rc s2rc s2del2027 s1upd2025 s2c s1c s1q

# Workaround:
# s1 updates the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock2027 s2del2027 s1lock2025 s2c s1upd2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be updated either.
permutation s1rc s2rc s2del202503 s1upd2025 s2c s1c s1q

# Workaround:
# s1 sees the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock202503 s2del202503 s1lock2025 s2c s1upd2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be updated either.
permutation s1rc s2rc s2del20252026 s1upd2025 s2c s1c s1q

# Workaround:
# s1 sees the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock20252026 s2del20252026 s1lock2025 s2c s1upd2025 s1c s1q

# ########################################
# READ COMMITTED tests, DELETE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1rc s2rc s2lock2027 s2upd2027 s2c s1lock2025 s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rc s2rc s2lock202503 s2upd202503 s2c s1lock2025 s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rc s2rc s2lock20252026 s2upd20252026 s2c s1lock2025 s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock2027 s2upd2027 s2c s1q

# s2 ignores the deleted row
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock202503 s2upd202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock20252026 s2upd20252026 s2c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be deleted either.
permutation s1rc s2rc s2upd2027 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock2027 s2upd2027 s1lock2025 s2c s1del2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# but EvalPlanQual still matches the row to be deleted.
permutation s1rc s2rc s2upd202503 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the new row from s2 and its leftovers
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock202503 s2upd202503 s1lock2025 s2c s1del2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# but EvalPlanQual still matches the row to be deleted,
# and s1 leaves leftovers from the row created by s2.
permutation s1rc s2rc s2upd20252026 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the new row from s2 and its leftovers
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock20252026 s2upd20252026 s1lock2025 s2c s1del2025 s1c s1q

# ########################################
# READ COMMITTED tests, DELETE+DELETE:
# ########################################

# s1 sees the leftovers
permutation s1rc s2rc s2lock2027 s2del2027 s2c s1lock2025 s1del2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s2lock202503 s2del202503 s2c s1lock2025 s1del2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s2lock20252026 s2del20252026 s2c s1lock2025 s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock2027 s2del2027 s2c s1q

# s2 ignores the deleted row
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock202503 s2del202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock20252026 s2del20252026 s2c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be deleted either.
permutation s1rc s2rc s2del2027 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock2027 s2del2027 s1lock2025 s2c s1del2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be deleted either.
permutation s1rc s2rc s2del202503 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock202503 s2del202503 s1lock2025 s2c s1del2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be deleted either.
permutation s1rc s2rc s2del20252026 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock20252026 s2del20252026 s1lock2025 s2c s1del2025 s1c s1q

# ########################################
# REPEATABLE READ tests, UPDATE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1rr s2rr s2upd2027 s2c s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2upd202503 s2c s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2upd20252026 s2c s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1upd2025 s1c s2upd2027 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1upd2025 s1c s2upd202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1upd2025 s1c s2upd20252026 s2c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd2027 s1upd2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd202503 s1upd2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd20252026 s1upd2025 s2c s1c s1q

## with prior read by s1:

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd2027 s2c s1upd2025 s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd202503 s2c s1upd2025 s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd20252026 s2c s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1q s1upd2025 s1c s2upd2027 s2c s1q

# s2 loads the updated row
permutation s1rr s2rr s1q s1upd2025 s1c s2upd202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1q s1upd2025 s1c s2upd20252026 s2c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd2027 s1upd2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd202503 s1upd2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd20252026 s1upd2025 s2c s1c s1q

# ########################################
# REPEATABLE READ tests, UPDATE+DELETE:
# ########################################

# s1 sees the leftovers
permutation s1rr s2rr s2del2027 s2c s1upd2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s2del202503 s2c s1upd2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s2del20252026 s2c s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1upd2025 s1c s2del2027 s2c s1q

# s2 loads the updated row
permutation s1rr s2rr s1upd2025 s1c s2del202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1upd2025 s1c s2del20252026 s2c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del2027 s1upd2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del202503 s1upd2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del20252026 s1upd2025 s2c s1c s1q

## with prior read by s1:

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del2027 s2c s1upd2025 s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del202503 s2c s1upd2025 s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del20252026 s2c s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1q s1upd2025 s1c s2del2027 s2c s1q

# s2 loads the updated row
permutation s1rr s2rr s1q s1upd2025 s1c s2del202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1q s1upd2025 s1c s2del20252026 s2c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del2027 s1upd2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del202503 s1upd2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del20252026 s1upd2025 s2c s1c s1q

# ########################################
# REPEATABLE READ tests, DELETE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1rr s2rr s2upd2027 s2c s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2upd202503 s2c s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2upd20252026 s2c s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1del2025 s1c s2upd2027 s2c s1q

# s2 ignores the deleted row
permutation s1rr s2rr s1del2025 s1c s2upd202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s1del2025 s1c s2upd20252026 s2c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd2027 s1del2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd202503 s1del2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd20252026 s1del2025 s2c s1c s1q

## with prior read by s1:

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd2027 s2c s1del2025 s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd202503 s2c s1del2025 s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd20252026 s2c s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1q s1del2025 s1c s2upd2027 s2c s1q

# s2 ignores the deleted row
permutation s1rr s2rr s1q s1del2025 s1c s2upd202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s1q s1del2025 s1c s2upd20252026 s2c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd2027 s1del2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd202503 s1del2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd20252026 s1del2025 s2c s1c s1q

# ########################################
# REPEATABLE READ tests, DELETE+DELETE:
# ########################################

# s1 sees the leftovers
permutation s1rr s2rr s2del2027 s2c s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2del202503 s2c s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2del20252026 s2c s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1del2025 s1c s2del2027 s2c s1q

# s2 ignores the deleted row
permutation s1rr s2rr s1del2025 s1c s2del202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s1del2025 s1c s2del20252026 s2c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del2027 s1del2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del202503 s1del2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del20252026 s1del2025 s2c s1c s1q

## with prior read by s1:

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del2027 s2c s1del2025 s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del202503 s2c s1del2025 s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del20252026 s2c s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1q s1del2025 s1c s2del2027 s2c s1q

# s2 ignores the deleted row
permutation s1rr s2rr s1q s1del2025 s1c s2del202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s1q s1del2025 s1c s2del20252026 s2c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del2027 s1del2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del202503 s1del2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del20252026 s1del2025 s2c s1c s1q

# ########################################
# SERIALIZABLE tests, UPDATE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1ser s2ser s2upd2027 s2c s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1ser s2ser s2upd20252026 s2c s1upd2025 s1c s1q
