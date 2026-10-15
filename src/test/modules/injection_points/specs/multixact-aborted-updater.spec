# Test concurrent behavior of transaction abort in the window between marking
# the transaction as aborted in pg_xact and removing it from the ProcArray.
#
# In most cases, the correct way to check if a transaction aborted is to first
# check TransactionIdIsInProgress(), followed by !TransactionDidCommit().  The
# TransactionIdIsInProgress() call ensures that the transaction is only
# considered as aborted after it's been removed from the ProcArray, and using
# !TransactionDidCommit() instead of TransactionIdDidAbort() ensures that you
# treat crashed transactions -- i.e. transactions that were in progress when the
# system crashed and didn't write an ABORT WAL record -- as aborted too.
#
# This test uses an injection point to pause an aborting transaction between
# updating pg_xact and removing it from the ProcArray, and performs different
# concurrent operations involving multixids.  This exercises various codepaths
# where the correct ordering of TransactionIdIsInProgress() and
# TransactionIdDidCommit() matters.

setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE mxact_abort (id int PRIMARY KEY, filler text);
	INSERT INTO mxact_abort VALUES (1, 'initial');
}

teardown
{
	DROP TABLE mxact_abort;
	DROP EXTENSION injection_points;
}

# Transaction 1 performs an UPDATE, aborts, and pauses between updating pg_xact
# and ProcArray cleanup
session s1
setup	{
	SELECT FROM injection_points_set_local();
	SELECT FROM injection_points_attach('transaction-abort-after-clog', 'wait');
}
step s1begin	{ BEGIN; }
step s1update	{ UPDATE mxact_abort SET filler = 's1' WHERE id = 1; }
step s1abort	{ ROLLBACK; }

# Transaction 2 locks the row in KEY SHARE mode, to cause a multixact to be
# created.
session s2
step s2lock		{ SELECT * FROM mxact_abort WHERE id = 1 FOR KEY SHARE; }

# Transaction 3 performs a concurrent operation while transaction 1 is aborting
session s3
step s3nowait	{ SELECT * FROM mxact_abort WHERE id = 1 FOR NO KEY UPDATE NOWAIT; }
step s3update	{ UPDATE mxact_abort SET filler = 's3' WHERE id = 1; }
step s3delete	{ DELETE FROM mxact_abort WHERE id = 1; }
step s3keyupdate	{ UPDATE mxact_abort SET id = 2 WHERE id = 1; }
step s3check	{ SELECT * FROM mxact_abort ORDER BY id; }

session waker
step wake		{
	SELECT FROM injection_points_detach('transaction-abort-after-clog');
	SELECT FROM injection_points_wakeup('transaction-abort-after-clog');
}

# A SELECT .. NOWAIT while the updating transaction is paused in the abort
# considers the updater as still running, and reports an error.
permutation
	s1begin
	s1update
	s2lock	   # succeeds and creates a multixid
	s1abort    # blocks on the injection point
	# fails, the aborting updater is still considered running
	s3nowait
	wake

# A conflicting UPDATE waits for the first updater to fully finish.
#
# Report the abort after the conflicting operation in these permutations
# to keep the completion output stable.
permutation
	s1begin
	s1update
	s2lock
	s1abort(s3update)
	# blocks waiting for the aborting updater to finish
	s3update
	wake

# Same for a DELETE
permutation
	s1begin
	s1update
	s2lock
	s1abort(s3delete)
	# blocks waiting for the aborting updater to finish
	s3delete
	wake
	s3check

# A key-changing UPDATE must also wait before deciding which members of
# the multixact survive.
permutation
	s1begin
	s1update
	s2lock
	s1abort(s3keyupdate)
	# blocks waiting for the aborting updater to finish
	s3keyupdate
	wake
	s3check
