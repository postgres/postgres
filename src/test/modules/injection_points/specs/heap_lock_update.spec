# Test race condition in tuple locking
#
# This is a reproducer for the bug reported at:
# https://www.postgresql.org/message-id/CAOG%2BRQ74x0q%3DkgBBQ%3DmezuvOeZBfSxM1qu_o0V28bwDz3dHxLw%40mail.gmail.com
#
# The bug was that when following an update chain when locking tuples,
# we sometimes failed to check that the xmin on the next tuple matched
# the prior's xmax. If the updated tuple version was vacuumed away and
# the slot was reused for an unrelated tuple, we'd incorrectly follow
# and lock the unrelated tuple.


# Set up a test table with enough rows to fill a page. We need the
# UPDATE used in the test to put the new tuple on a different page,
# because otherwise the VACUUM cannot remove the aborted tuple because
# we hold a pin on the first page.
#
# The exact number of rows inserted doesn't otherwise matter, but we
# arrange things in a deterministic fashion so that the last inserted
# tuple goes to (1,1), and the updated and aborted tuple goes to
# (1,2). That way we can just memorize those ctids in the expected
# output, to verify that the test exercises the scenario we want.
setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE t (id int PRIMARY KEY);
	do $$
		DECLARE
			i int;
			tid tid;
		BEGIN
			FOR i IN 1..5000 LOOP
				INSERT INTO t VALUES (i) RETURNING ctid INTO tid;
				IF tid = '(1,1)' THEN
					RETURN;
				END IF;
			END LOOP;
			RAISE 'expected to insert tuple to (1,1)';
	   END;
   $$;
}
teardown
{
	DROP TABLE t;
	DROP EXTENSION injection_points;
}

session s1
step s1begin	{ BEGIN; }
step s1update	{ UPDATE t SET id = 10000 WHERE id = 1 RETURNING ctid; }
step s1abort	{ ABORT; }
step vacuum		{ VACUUM t; }

# Insert a new tuple, and update it.
step reinsert	{
	INSERT INTO t VALUES (10001) RETURNING ctid;
	UPDATE t SET id = 10002 WHERE id = 10001 RETURNING ctid;
}

# Same as the 'reinsert' step, but for extra confusion, we also stamp
# the original tuple with the same 'xmax' as the re-inserted one.
step reinsert_and_lock {
	BEGIN;
	INSERT INTO t VALUES (10001) RETURNING ctid;
	SELECT ctid, * FROM t WHERE id = 1 FOR UPDATE;
	COMMIT;
	UPDATE t SET id = 10002 WHERE id = 10001 returning ctid;
}

step wake	{
	SELECT FROM injection_points_detach('heap_lock_updated_tuple');
	SELECT FROM injection_points_wakeup('heap_lock_updated_tuple');
}

session s2
setup	{
	SELECT FROM injection_points_set_local();
	SELECT FROM injection_points_attach('heap_lock_updated_tuple', 'wait');
}
step s2lock	{ select * from t where id = 1 for update; }

permutation
	# Begin transaction, update a row. Because of how we set up the
	# test table, the updated tuple lands at (1,2)
	s1begin
	s1update

	# While the updating transaction is open, start a new session that
	# tries to lock the row. This blocks on the open transaction.
	s2lock

	# Abort the updating transaction. This unblocks session 2, but it
	# will immediately hit the injection point and block on that.
	s1abort
	# Vacuum away the updated, aborted tuple.
	vacuum

	# Insert a new tuple. It lands at the same location where the
	# updated tuple was.
	reinsert

	# Let the locking transaction continue. It should lock the
	# original tuple, ignoring the re-inserted tuple.
	wake(s2lock)

# Variant where the re-inserted tuple is also locked by the inserting
# transaction. This failed an earlier version of the fix during
# development.
permutation
	s1begin
	s1update
	s2lock
	s1abort
	vacuum
	reinsert_and_lock
	wake(s2lock)
