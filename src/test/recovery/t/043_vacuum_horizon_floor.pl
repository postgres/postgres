use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More;

# Test that vacuum prunes away all dead tuples killed before OldestXmin
#
# This test creates a table on a primary, updates the table to generate dead
# tuples for vacuum, and then, during the vacuum, uses the replica to force
# GlobalVisState->maybe_needed on the primary to move backwards and precede
# the value of OldestXmin set at the beginning of vacuuming the table.

# Set up nodes
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 'physical');

$node_primary->append_conf(
	'postgresql.conf', qq[
hot_standby_feedback = on
autovacuum = off
log_min_messages = INFO
maintenance_work_mem = 1024
]);
$node_primary->start;

my $node_replica = PostgreSQL::Test::Cluster->new('standby');

$node_primary->backup('my_backup');
$node_replica->init_from_backup($node_primary, 'my_backup',
	has_streaming => 1);

$node_replica->start;

my $test_db = "test_db";
$node_primary->safe_psql('postgres', "CREATE DATABASE $test_db");

# Save the original connection info for later use
my $orig_conninfo = $node_primary->connstr();

my $table1 = "vac_horizon_floor_table";

# Long-running Primary Session A
my $psql_primaryA =
  $node_primary->background_psql($test_db, on_error_stop => 1);

# Long-running Primary Session B
my $psql_primaryB  =
  $node_primary->background_psql($test_db, on_error_stop => 1);

# Because vacuum's first pass, pruning, is where we use the GlobalVisState to
# check tuple visibility, GlobalVisState->maybe_needed must move backwards
# during pruning before checking the visibility for a tuple which would have
# been considered HEAPTUPLE_DEAD prior to maybe_needed moving backwards but
# HEAPTUPLE_RECENTLY_DEAD compared to the new, older value of maybe_needed.
#
# We must not only force the horizon on the primary to move backwards but also
# force the vacuuming backend's GlobalVisState to be updated. GlobalVisState
# is forced to update during index vacuuming.
#
# _bt_pendingfsm_finalize() calls GetOldestNonRemovableTransactionId() at the
# end of a round of index vacuuming, updating the backend's GlobalVisState
# and, in our case, moving maybe_needed backwards.
#
# Then vacuum's first (pruning) pass will continue and pruning will find our
# later inserted and updated tuple HEAPTUPLE_RECENTLY_DEAD when compared to
# maybe_needed but HEAPTUPLE_DEAD when compared to OldestXmin.
#
# Thus, we must force at least two rounds of index vacuuming to ensure that
# some tuple visibility checks will happen after a round of index vacuuming.
# To accomplish this, we set maintenance_work_mem to its minimum value and
# insert and update enough rows that we force at least one round of index
# vacuuming before getting to a dead tuple which was killed after the
# standby is disconnected.
$node_primary->safe_psql($test_db, qq[
	CREATE TABLE ${table1}(col1 int) with (autovacuum_enabled=false);
	INSERT INTO $table1 SELECT generate_series(1, 200000);
	CREATE INDEX on ${table1}(col1);
	DELETE FROM $table1 WHERE col1 > 1;
	INSERT INTO $table1 VALUES(1);
]);

# We will later move the primary forward while the standby is disconnected.
# For now, however, there is no reason not to wait for the standby to catch
# up.
my $primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_replica, 'replay', $primary_lsn);

# Test that the WAL receiver is up and running.
$node_replica->poll_query_until($test_db, qq[
	SELECT EXISTS (SELECT * FROM pg_stat_wal_receiver);] , 't');

# Set primary_conninfo to something invalid on the replica and reload the
# config. Once the config is reloaded, the startup process will force the WAL
# receiver to restart and it will be unable to reconnect because of the
# invalid connection information.
$node_replica->safe_psql($test_db, qq[
		ALTER SYSTEM SET primary_conninfo = '';
		SELECT pg_reload_conf();
	]);

# Wait until the WAL receiver has shut down and been unable to start up again.
$node_replica->poll_query_until($test_db, qq[
	SELECT EXISTS (SELECT * FROM pg_stat_wal_receiver);] , 'f');

# Now insert and update a tuple which will be visible to the vacuum on the
# primary but which will have xmax newer than the oldest xmin on the standby
# that was recently disconnected.
my $res = $psql_primaryA->query_safe(
	qq[
		INSERT INTO $table1 VALUES (99);
		UPDATE $table1 SET col1 = 100 WHERE col1 = 99;
		SELECT 'after_update';
        ]
	);

# Make sure the UPDATE finished
like($res, qr/^after_update$/m, "UPDATE occurred on primary session A");

# Open a cursor on the primary whose pin will keep VACUUM from getting a
# cleanup lock on the first page of the relation. We want VACUUM to be able to
# start, calculate initial values for OldestXmin and GlobalVisState and then
# be unable to proceed with pruning our dead tuples. This will allow us to
# reconnect the standby and push the horizon back before we start actual
# pruning and vacuuming.
my $primary_cursor1 = "vac_horizon_floor_cursor";

# The first value inserted into the table was a 1, so FETCH FORWARD should
# return a 1. That's how we know the cursor has a pin.
$res = $psql_primaryB->query_safe(
	qq[
	BEGIN;
	DECLARE $primary_cursor1 CURSOR FOR SELECT * FROM $table1 WHERE col1 = 1;
	FETCH FORWARD FROM $primary_cursor1;
	]
	);

is($res, 1, qq[Cursor query returned $res. Expected value 1.]);

# Get the PID of the session which will run the VACUUM FREEZE so that we can
# use it to filter pg_stat_activity later.
my $vacuum_pid = $psql_primaryA->query_safe("SELECT pg_backend_pid();");

# Now start a VACUUM FREEZE on the primary. It will call vacuum_get_cutoffs()
# and establish values of OldestXmin and GlobalVisState which are newer than
# all of our dead tuples. Then it will be unable to get a cleanup lock to
# start pruning, so it will hang.
#
# We use VACUUM FREEZE because it will wait for a cleanup lock instead of
# skipping the page pinned by the cursor. Note that works because the target
# tuple's xmax precedes OldestXmin which ensures that lazy_scan_noprune() will
# return false and we will wait for the cleanup lock.
$psql_primaryA->{stdin} .= qq[
		VACUUM (VERBOSE, FREEZE) $table1;
		\\echo VACUUM
        ];

# Make sure the VACUUM command makes it to the server.
$psql_primaryA->{run}->pump_nb();

# Make sure that the VACUUM has already called vacuum_get_cutoffs() and is
# just waiting on the lock to start vacuuming. We don't want the standby to
# re-establish a connection to the primary and push the horizon back until
# we've saved initial values in GlobalVisState and calculated OldestXmin.
$node_primary->poll_query_until($test_db,
	qq[
	SELECT count(*) >= 1 FROM pg_stat_activity
		WHERE pid = $vacuum_pid
		AND wait_event = 'BufferPin';
	],
	't');

# Ensure the WAL receiver is still not active on the replica.
$node_replica->poll_query_until($test_db, qq[
	SELECT EXISTS (SELECT * FROM pg_stat_wal_receiver);] , 'f');

# Allow the WAL receiver connection to re-establish.
$node_replica->safe_psql(
	$test_db, qq[
		ALTER SYSTEM SET primary_conninfo = '$orig_conninfo';
		SELECT pg_reload_conf();
	]);

# Ensure the new WAL receiver has connected.
$node_replica->poll_query_until($test_db, qq[
	SELECT EXISTS (SELECT * FROM pg_stat_wal_receiver);] , 't');

# Once the WAL sender is shown on the primary, the replica should have
# connected with the primary and pushed the horizon backward. Primary Session
# A won't see that until the VACUUM FREEZE proceeds and does its first round
# of index vacuuming.
$node_primary->poll_query_until($test_db, qq[
	SELECT EXISTS (SELECT * FROM pg_stat_replication);] , 't');

# Move the cursor forward to the next 1. We inserted the 1 much later, so
# advancing the cursor should allow vacuum to proceed vacuuming most pages of
# the relation. Because we set maintanence_work_mem sufficiently low, we
# expect that a round of index vacuuming has happened and that the vacuum is
# now waiting for the cursor to release its pin on the last page of the
# relation.
$res = $psql_primaryB->query_safe("FETCH $primary_cursor1");
is($res, 1,
	qq[Cursor query returned $res from second fetch. Expected value 1.]);

# Prevent the test from incorrectly passing by confirming that we did indeed
# do a pass of index vacuuming.
$node_primary->poll_query_until($test_db, qq[
	SELECT index_vacuum_count > 0
	FROM pg_stat_progress_vacuum
	WHERE datname='$test_db' AND relid::regclass = '$table1'::regclass;
	] , 't');

# Commit the transaction with the open cursor so that the VACUUM can finish.
$psql_primaryB->query_until(
		qr/^commit$/m,
		qq[
			COMMIT;
			\\echo commit
        ]
	);

# VACUUM proceeds with pruning and does a visibility check on each tuple. In
# older versions of Postgres, pruning found our final dead tuple
# non-removable (HEAPTUPLE_RECENTLY_DEAD) since its xmax is after the new
# value of maybe_needed. Then lazy_scan_prune() would infinitely loop
# because HeapTupleSatisfiesVacuum() would find the tuple HEAPTUPLE_DEAD
# because its xmax preceded OldestXmin. This was fixed by passing
# OldestXmin to heap_page_prune() and removing all tuples whose xmaxes
# precede OldestXmin.
#
# With the fix, VACUUM should finish successfully, incrementing the table
# vacuum_count.
$node_primary->poll_query_until($test_db,
	qq[
	SELECT vacuum_count > 0
	FROM pg_stat_all_tables WHERE relname = '${table1}';
	]
	, 't');

$primary_lsn = $node_primary->lsn('flush');

# Make sure something causes us to flush
$node_primary->safe_psql($test_db, "INSERT INTO $table1 VALUES (1);");

# Nothing on the replica should cause a recovery conflict, so this should
# finish successfully.
$node_primary->wait_for_catchup($node_replica, 'replay', $primary_lsn);

## Shut down psqls
$psql_primaryA->quit;
$psql_primaryB->quit;

$node_replica->stop();
$node_primary->stop();

done_testing();
