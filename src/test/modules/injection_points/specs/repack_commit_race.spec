# Test that the logical decoding initial snapshot build, used for
# REPACK (CONCURRENTLY), waits for transactions to remove themselves
# from ProcArray.  Transactions are initially considered committed when
# their commit WAL records are decoded, but that's not sufficient
# for MVCC correctness.
setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE repack_race(i int PRIMARY KEY, j int);
	INSERT INTO repack_race(i, j) VALUES (1, 1), (2, 2);
}

teardown
{
	DROP TABLE repack_race;
	DROP EXTENSION injection_points;
}

session s1
step s1_repack
{
	REPACK (CONCURRENTLY) repack_race;
}
step s1_check
{
	SELECT i, j FROM repack_race ORDER BY i;
}

# s2 and s3 keep a transaction with an XID open, so that the snapshot builder
# has to go through its BUILDING_SNAPSHOT and FULL_SNAPSHOT states instead of
# becoming consistent right away.
session s2
step s2_begin
{
	BEGIN;
	SELECT pg_current_xact_id() IS NOT NULL;
}
step s2_rollback
{
	ROLLBACK;
}

session s3
step s3_begin
{
	BEGIN;
	SELECT pg_current_xact_id() IS NOT NULL;
}
step s3_rollback
{
	ROLLBACK;
}

# s4 changes the table and stops after writing its commit record, before
# updating CLOG.
session s4
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('commit-before-clog-update', 'wait');
}
step s4_changes
{
	INSERT INTO repack_race(i, j) VALUES (3, 3);
	UPDATE repack_race SET j = j + 1 WHERE i = 1;
	DELETE FROM repack_race WHERE i = 2;
}
teardown
{
	SELECT injection_points_detach('commit-before-clog-update');
}

session s5
step s5_wakeup
{
	SELECT injection_points_wakeup('commit-before-clog-update');
}

# The snapshot builder waits for s2, then for s3.  While it waits for s3, s4
# writes its commit record: the builder will count s4 as committed and start
# decoding after it, but CLOG does not know about s4 yet.  REPACK must not use
# its snapshot before s4 has finished committing.
#
# s4 cannot finish before s5 wakes it up, and s1 cannot finish before s4 does;
# the marker on s4_changes keeps the reporting order stable.
permutation
	s2_begin
	s1_repack
	s3_begin
	s2_rollback
	s4_changes(s1_repack)
	s3_rollback
	s5_wakeup
	s1_check
