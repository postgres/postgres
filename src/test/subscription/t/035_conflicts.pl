# Copyright (c) 2025-2026, PostgreSQL Global Development Group

# Test conflicts in logical replication
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

###############################
# Setup
###############################

# Create a publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create a subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Create a table on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE conf_tab (a int PRIMARY KEY, b int UNIQUE, c int UNIQUE);");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE conf_tab_2 (a int PRIMARY KEY, b int UNIQUE, c int UNIQUE);"
);

# Create same table on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE conf_tab (a int PRIMARY key, b int UNIQUE, c int UNIQUE);");

$node_subscriber->safe_psql(
	'postgres', qq[
	 CREATE TABLE conf_tab_2 (a int PRIMARY KEY, b int, c int, unique(a,b)) PARTITION BY RANGE (a);
	 CREATE TABLE conf_tab_2_p1 PARTITION OF conf_tab_2 FOR VALUES FROM (MINVALUE) TO (100);
]);

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_tab FOR TABLE conf_tab, conf_tab_2");

# Create the subscription
my $appname = 'sub_tab';
$node_subscriber->safe_psql(
	'postgres',
	"CREATE SUBSCRIPTION sub_tab
	 CONNECTION '$publisher_connstr application_name=$appname'
	 PUBLICATION pub_tab;");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

##################################################
# INSERT data on Pub and Sub
##################################################

# Insert data in the publisher table
$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (1,1,1);");

# Insert data in the subscriber table
$node_subscriber->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (2,2,2), (3,3,3), (4,4,4);");

##################################################
# Test multiple_unique_conflicts due to INSERT
##################################################
my $log_offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (2,3,4);");

# Confirm that this causes an error on the subscriber
$node_subscriber->wait_for_log(
	qr/conflict detected on relation \"public.conf_tab\": conflict=multiple_unique_conflicts.*
.*Could not apply remote change: remote row \(2, 3, 4\).*
.*Key already exists in unique index \"conf_tab_pkey\", modified in transaction .*: key \(a\)=\(2\), local row \(2, 2, 2\).*
.*Key already exists in unique index \"conf_tab_b_key\", modified in transaction .*: key \(b\)=\(3\), local row \(3, 3, 3\).*
.*Key already exists in unique index \"conf_tab_c_key\", modified in transaction .*: key \(c\)=\(4\), local row \(4, 4, 4\)./,
	$log_offset);

pass('multiple_unique_conflicts detected during insert');

# Truncate table to get rid of the error
$node_subscriber->safe_psql('postgres', "TRUNCATE conf_tab;");

##################################################
# Test multiple_unique_conflicts due to UPDATE
##################################################
$log_offset = -s $node_subscriber->logfile;

# Insert data in the publisher table
$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (5,5,5);");

# Insert data in the subscriber table
$node_subscriber->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (6,6,6), (7,7,7), (8,8,8);");

$node_publisher->safe_psql('postgres',
	"UPDATE conf_tab set a=6, b=7, c=8 where a=5;");

# Confirm that this causes an error on the subscriber
$node_subscriber->wait_for_log(
	qr/conflict detected on relation \"public.conf_tab\": conflict=multiple_unique_conflicts.*
.*Could not apply remote change: remote row \(6, 7, 8\), replica identity \(a\)=\(5\).*
.*Key already exists in unique index \"conf_tab_pkey\", modified in transaction .*: key \(a\)=\(6\), local row \(6, 6, 6\).*
.*Key already exists in unique index \"conf_tab_b_key\", modified in transaction .*: key \(b\)=\(7\), local row \(7, 7, 7\).*
.*Key already exists in unique index \"conf_tab_c_key\", modified in transaction .*: key \(c\)=\(8\), local row \(8, 8, 8\)./,
	$log_offset);

pass('multiple_unique_conflicts detected during update');

# Truncate table to get rid of the error
$node_subscriber->safe_psql('postgres', "TRUNCATE conf_tab;");


##################################################
# Test multiple_unique_conflicts due to INSERT on a leaf partition
##################################################

# Insert data in the subscriber table
$node_subscriber->safe_psql('postgres',
	"INSERT INTO conf_tab_2 VALUES (55,2,3);");

# Insert data in the publisher table
$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab_2 VALUES (55,2,3);");

$node_subscriber->wait_for_log(
	qr/conflict detected on relation \"public.conf_tab_2_p1\": conflict=multiple_unique_conflicts.*
.*Could not apply remote change: remote row \(55, 2, 3\).*
.*Key already exists in unique index \"conf_tab_2_p1_pkey\", modified in transaction .*: key \(a\)=\(55\), local row \(55, 2, 3\).*
.*Key already exists in unique index \"conf_tab_2_p1_a_b_key\", modified in transaction .*: key \(a, b\)=\(55, 2\), local row \(55, 2, 3\)./,
	$log_offset);

pass('multiple_unique_conflicts detected on a leaf partition during insert');

###############################################################################
# Setup a bidirectional logical replication between node_A & node_B
###############################################################################

# Initialize nodes. Enable the track_commit_timestamp on both nodes to detect
# the conflict when attempting to update a row that was previously modified by
# a different origin.

# node_A. Increase the log_min_messages setting to DEBUG2 to debug test
# failures. Disable autovacuum to avoid generating xid that could affect the
# replication slot's xmin value.
my $node_A = $node_publisher;
$node_A->append_conf(
	'postgresql.conf',
	qq{track_commit_timestamp = on
	autovacuum = off
	log_min_messages = 'debug2'});
$node_A->restart;

# node_B
my $node_B = $node_subscriber;
$node_B->append_conf('postgresql.conf', "track_commit_timestamp = on");
$node_B->restart;

# Create table on node_A
$node_A->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY, b int)");

# Create the same table on node_B
$node_B->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY, b int)");

my $subname_AB = 'tap_sub_a_b';
my $subname_BA = 'tap_sub_b_a';

# Setup logical replication
# node_A (pub) -> node_B (sub)
my $node_A_connstr = $node_A->connstr . ' dbname=postgres';
$node_A->safe_psql('postgres', "CREATE PUBLICATION tap_pub_A FOR TABLE tab");
$node_B->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION $subname_BA
	CONNECTION '$node_A_connstr application_name=$subname_BA'
	PUBLICATION tap_pub_A
	WITH (origin = none, retain_dead_tuples = true)");

# node_B (pub) -> node_A (sub)
my $node_B_connstr = $node_B->connstr . ' dbname=postgres';
$node_B->safe_psql('postgres', "CREATE PUBLICATION tap_pub_B FOR TABLE tab");
$node_A->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION $subname_AB
	CONNECTION '$node_B_connstr application_name=$subname_AB'
	PUBLICATION tap_pub_B
	WITH (origin = none, copy_data = off)");

# Wait for initial table sync to finish
$node_A->wait_for_subscription_sync($node_B, $subname_AB);
$node_B->wait_for_subscription_sync($node_A, $subname_BA);

is(1, 1, 'Bidirectional replication setup is complete');

# Confirm that the conflict detection slot is created on Node B and the xmin
# value is valid.
ok( $node_B->poll_query_until(
		'postgres',
		"SELECT xmin IS NOT NULL from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the xmin value of slot 'pg_conflict_detection' is valid on Node B");

##################################################
# Check that the retain_dead_tuples option can be enabled only for disabled
# subscriptions. Validate the NOTICE message during the subscription DDL, and
# ensure the conflict detection slot is created upon enabling the
# retain_dead_tuples option.
##################################################

# Alter retain_dead_tuples for enabled subscription
my ($cmdret, $stdout, $stderr) = $node_A->psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (retain_dead_tuples = true)");
like(
	$stderr,
	qr/ERROR:  cannot set option \"retain_dead_tuples\" for enabled subscription/,
	"altering retain_dead_tuples is not allowed for enabled subscription");

# Disable the subscription
$node_A->psql('postgres', "ALTER SUBSCRIPTION $subname_AB DISABLE;");

# Wait for the apply worker to stop
$node_A->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);

# Enable retain_dead_tuples for disabled subscription
($cmdret, $stdout, $stderr) = $node_A->psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (retain_dead_tuples = true);");
like(
	$stderr,
	qr/NOTICE:  deleted rows to detect conflicts would not be removed until the subscription is enabled/,
	"altering retain_dead_tuples is allowed for disabled subscription");

# Re-enable the subscription
$node_A->safe_psql('postgres', "ALTER SUBSCRIPTION $subname_AB ENABLE;");

# Confirm that the conflict detection slot is created on Node A and the xmin
# value is valid.
ok( $node_A->poll_query_until(
		'postgres',
		"SELECT xmin IS NOT NULL from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the xmin value of slot 'pg_conflict_detection' is valid on Node A");

##################################################
# Check the WARNING when changing the origin to ANY, if retain_dead_tuples is
# enabled. This warns of the possibility of receiving changes from origins
# other than the publisher.
##################################################

($cmdret, $stdout, $stderr) = $node_A->psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (origin = any);");
like(
	$stderr,
	qr/WARNING:  subscription "tap_sub_a_b" enabled retain_dead_tuples but might not reliably detect conflicts for changes from different origins/,
	"warn of the possibility of receiving changes from origins other than the publisher"
);

# Reset the origin to none
$node_A->psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (origin = none);");

###############################################################################
# Check that dead tuples on node A cannot be cleaned by VACUUM until the
# concurrent transactions on Node B have been applied and flushed on Node A.
# Also, check that an update_deleted conflict is detected when updating a row
# that was deleted by a different origin.
###############################################################################

# Insert a record
$node_A->safe_psql('postgres', "INSERT INTO tab VALUES (1, 1), (2, 2);");
$node_A->wait_for_catchup($subname_BA);

my $result = $node_B->safe_psql('postgres', "SELECT * FROM tab;");
is($result, qq(1|1
2|2), 'check replicated insert on node B');

# Disable the logical replication from node B to node A
$node_A->safe_psql('postgres', "ALTER SUBSCRIPTION $subname_AB DISABLE");

# Wait for the apply worker to stop
$node_A->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);

my $log_location = -s $node_B->logfile;

$node_B->safe_psql('postgres', "UPDATE tab SET b = 3 WHERE a = 1;");
$node_A->safe_psql('postgres', "DELETE FROM tab WHERE a = 1;");

($cmdret, $stdout, $stderr) = $node_A->psql(
	'postgres', qq(VACUUM (verbose) public.tab;)
);

like(
	$stderr,
	qr/1 are dead but not yet removable/,
	'the deleted column is non-removable');

# Ensure the DELETE is replayed on Node B
$node_A->wait_for_catchup($subname_BA);

# Check the conflict detected on Node B
my $logfile = slurp_file($node_B->logfile(), $log_location);
like(
	$logfile,
	qr/conflict detected on relation "public.tab": conflict=delete_origin_differs.*
.*DETAIL:.* Deleting the row that was modified locally in transaction [0-9]+ at .*: local row \(1, 3\), replica identity \(a\)=\(1\)./,
	'delete target row was modified in tab');

$log_location = -s $node_A->logfile;

$node_A->safe_psql(
	'postgres', "ALTER SUBSCRIPTION $subname_AB ENABLE;");
$node_B->wait_for_catchup($subname_AB);

$logfile = slurp_file($node_A->logfile(), $log_location);
like(
	$logfile,
	qr/conflict detected on relation "public.tab": conflict=update_deleted.*
.*DETAIL:.* Could not find the row to be updated: remote row \(1, 3\), replica identity \(a\)=\(1\).
.*The row to be updated was deleted locally in transaction [0-9]+ at .*/,
	'update target row was deleted in tab');

# Remember the next transaction ID to be assigned
my $next_xid = $node_A->safe_psql('postgres', "SELECT txid_current() + 1;");

# Confirm that the xmin value is advanced to the latest nextXid. If no
# transactions are running, the apply worker selects nextXid as the candidate
# for the non-removable xid. See GetOldestActiveTransactionId().
ok( $node_A->poll_query_until(
		'postgres',
		"SELECT xmin = $next_xid from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the xmin value of slot 'pg_conflict_detection' is updated on Node A");

###############################################################################
# Ensure that the deleted tuple needed to detect an update_deleted conflict is
# accessible via a sequential table scan.
###############################################################################

# Drop the primary key from tab on node A and set REPLICA IDENTITY to FULL to
# enforce sequential scanning of the table.
$node_A->safe_psql('postgres', "ALTER TABLE tab REPLICA IDENTITY FULL");
$node_B->safe_psql('postgres', "ALTER TABLE tab REPLICA IDENTITY FULL");
$node_A->safe_psql('postgres', "ALTER TABLE tab DROP CONSTRAINT tab_pkey;");

# Disable the logical replication from node B to node A
$node_A->safe_psql('postgres', "ALTER SUBSCRIPTION $subname_AB DISABLE");

# Wait for the apply worker to stop
$node_A->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);

$node_B->safe_psql('postgres', "UPDATE tab SET b = 4 WHERE a = 2;");
$node_A->safe_psql('postgres', "DELETE FROM tab WHERE a = 2;");

$log_location = -s $node_A->logfile;

$node_A->safe_psql(
	'postgres', "ALTER SUBSCRIPTION $subname_AB ENABLE;");
$node_B->wait_for_catchup($subname_AB);

$logfile = slurp_file($node_A->logfile(), $log_location);
like(
	$logfile,
	qr/conflict detected on relation "public.tab": conflict=update_deleted.*
.*DETAIL:.* Could not find the row to be updated: remote row \(2, 4\), replica identity full \(2, 2\).*
.*The row to be updated was deleted locally in transaction [0-9]+ at .*/,
	'update target row was deleted in tab');

###############################################################################
# Check that the xmin value of the conflict detection slot can be advanced when
# the subscription has no tables.
###############################################################################

# Remove the table from the publication
$node_B->safe_psql('postgres', "ALTER PUBLICATION tap_pub_B DROP TABLE tab");

$node_A->safe_psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB REFRESH PUBLICATION");

# Remember the next transaction ID to be assigned
$next_xid = $node_A->safe_psql('postgres', "SELECT txid_current() + 1;");

# Confirm that the xmin value is advanced to the latest nextXid. If no
# transactions are running, the apply worker selects nextXid as the candidate
# for the non-removable xid. See GetOldestActiveTransactionId().
ok( $node_A->poll_query_until(
		'postgres',
		"SELECT xmin = $next_xid from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the xmin value of slot 'pg_conflict_detection' is updated on Node A");

# Re-add the table to the publication for further tests
$node_B->safe_psql('postgres', "ALTER PUBLICATION tap_pub_B ADD TABLE tab");

$node_A->safe_psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB REFRESH PUBLICATION WITH (copy_data = false)");

###############################################################################
# Test that publisher's transactions marked with DELAY_CHKPT_IN_COMMIT prevent
# concurrently deleted tuples on the subscriber from being removed. This test
# also acts as a safeguard to prevent developers from moving the commit
# timestamp acquisition before marking DELAY_CHKPT_IN_COMMIT in
# RecordTransactionCommitPrepared.
###############################################################################

my $injection_points_supported = $node_B->check_extension('injection_points');

# This test depends on an injection point to block the prepared transaction
# commit after marking DELAY_CHKPT_IN_COMMIT flag.
if ($injection_points_supported != 0)
{
	$node_B->append_conf('postgresql.conf',
		"shared_preload_libraries = 'injection_points'
		max_prepared_transactions = 1");
	$node_B->restart;

	# Disable the subscription on Node B for testing only one-way
	# replication.
	$node_B->psql('postgres', "ALTER SUBSCRIPTION $subname_BA DISABLE;");

	# Wait for the apply worker to stop
	$node_B->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
	);

	# Truncate the table to cleanup existing dead rows in the table. Then insert
	# a new row.
	$node_B->safe_psql(
		'postgres', qq(
		TRUNCATE tab;
		INSERT INTO tab VALUES(1, 1);
	));

	$node_B->wait_for_catchup($subname_AB);

	# Create the injection_points extension on the publisher node and attach to the
	# commit-after-delay-checkpoint injection point.
	$node_B->safe_psql(
		'postgres',
		"CREATE EXTENSION injection_points;
		 SELECT injection_points_attach('commit-after-delay-checkpoint', 'wait');"
	);

	# Start a background session on the publisher node to perform an update and
	# pause at the injection point.
	my $pub_session = $node_B->background_psql('postgres');
	$pub_session->query_until(
		qr/starting_bg_psql/,
		q{
			\echo starting_bg_psql
			BEGIN;
			UPDATE tab SET b = 2 WHERE a = 1;
			PREPARE TRANSACTION 'txn_with_later_commit_ts';
			COMMIT PREPARED 'txn_with_later_commit_ts';
		}
	);

	# Wait until the backend enters the injection point
	$node_B->wait_for_event('client backend', 'commit-after-delay-checkpoint');

	# Confirm the update is suspended
	$result =
	  $node_B->safe_psql('postgres', 'SELECT * FROM tab WHERE a = 1');
	is($result, qq(1|1), 'publisher sees the old row');

	# Delete the row on the subscriber. The deleted row should be retained due to a
	# transaction on the publisher, which is currently marked with the
	# DELAY_CHKPT_IN_COMMIT flag.
	$node_A->safe_psql('postgres', "DELETE FROM tab WHERE a = 1;");

	# Get the commit timestamp for the delete
	my $sub_ts = $node_A->safe_psql('postgres',
		"SELECT timestamp FROM pg_last_committed_xact();");

	$log_location = -s $node_A->logfile;

	# Confirm that the apply worker keeps requesting publisher status, while
	# awaiting the prepared transaction to commit. Thus, the request log should
	# appear more than once.
	$node_A->wait_for_log(
		qr/sending publisher status request message/,
		$log_location);

	$log_location = -s $node_A->logfile;

	$node_A->wait_for_log(
		qr/sending publisher status request message/,
		$log_location);

	# Confirm that the dead tuple cannot be removed
	($cmdret, $stdout, $stderr) =
	  $node_A->psql('postgres', qq(VACUUM (verbose) public.tab;));

	like(
		$stderr,
		qr/1 are dead but not yet removable/,
		'the deleted column is non-removable');

	$log_location = -s $node_A->logfile;

	# Wakeup and detach the injection point on the publisher node. The prepared
	# transaction should now commit.
	$node_B->safe_psql(
		'postgres',
		"SELECT injection_points_wakeup('commit-after-delay-checkpoint');
		 SELECT injection_points_detach('commit-after-delay-checkpoint');"
	);

	# Close the background session on the publisher node
	ok($pub_session->quit, "close publisher session");

	# Confirm that the transaction committed
	$result =
	  $node_B->safe_psql('postgres', 'SELECT * FROM tab WHERE a = 1');
	is($result, qq(1|2), 'publisher sees the new row');

	# Ensure the UPDATE is replayed on subscriber
	$node_B->wait_for_catchup($subname_AB);

	$logfile = slurp_file($node_A->logfile(), $log_location);
	like(
		$logfile,
		qr/conflict detected on relation "public.tab": conflict=update_deleted.*
.*DETAIL:.* Could not find the row to be updated: remote row \(1, 2\), replica identity full \(1, 1\).*
.*The row to be updated was deleted locally in transaction [0-9]+ at .*/,
		'update target row was deleted in tab');

	# Remember the next transaction ID to be assigned
	$next_xid =
	  $node_A->safe_psql('postgres', "SELECT txid_current() + 1;");

	# Confirm that the xmin value is advanced to the latest nextXid after the
	# prepared transaction on the publisher has been committed.
	ok( $node_A->poll_query_until(
			'postgres',
			"SELECT xmin = $next_xid from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
		),
		"the xmin value of slot 'pg_conflict_detection' is updated on subscriber"
	);

	# Get the commit timestamp for the publisher's update
	my $pub_ts = $node_B->safe_psql('postgres',
		"SELECT pg_xact_commit_timestamp(xmin) from tab where a=1;");

	# Check that the commit timestamp for the update on the publisher is later than
	# or equal to the timestamp of the local deletion, as the commit timestamp
	# should be assigned after marking the DELAY_CHKPT_IN_COMMIT flag.
	$result = $node_B->safe_psql('postgres',
		"SELECT '$pub_ts'::timestamp >= '$sub_ts'::timestamp");
	is($result, qq(t),
		"pub UPDATE's timestamp is later than that of sub's DELETE");

	# Re-enable the subscription for further tests
	$node_B->psql('postgres', "ALTER SUBSCRIPTION $subname_BA ENABLE;");
}

###############################################################################
# Check that dead tuple retention stops due to the wait time surpassing
# max_retention_duration.
###############################################################################

# Create a physical slot
$node_B->safe_psql('postgres',
	"SELECT * FROM pg_create_physical_replication_slot('blocker');");

# Add the inactive physical slot to synchronized_standby_slots
$node_B->append_conf('postgresql.conf',
	"synchronized_standby_slots = 'blocker'");
$node_B->reload;

# Enable failover to activate the synchronized_standby_slots setting
$node_A->safe_psql('postgres', "ALTER SUBSCRIPTION $subname_AB DISABLE;");
$node_A->safe_psql('postgres', "ALTER SUBSCRIPTION $subname_AB SET (failover = true);");
$node_A->safe_psql('postgres', "ALTER SUBSCRIPTION $subname_AB ENABLE;");

# Insert a record
$node_B->safe_psql('postgres', "INSERT INTO tab VALUES (5, 5);");

# Advance the xid on Node A to trigger the next cycle of oldest_nonremovable_xid
# advancement.
$node_A->safe_psql('postgres', "SELECT txid_current() + 1;");

$log_offset = -s $node_A->logfile;

# Set max_retention_duration to a minimal value to initiate retention stop.
$node_A->safe_psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (max_retention_duration = 1);");

# Confirm that the retention is stopped
$node_A->wait_for_log(
	qr/logical replication worker for subscription "tap_sub_a_b" has stopped retaining the information for detecting conflicts/,
	$log_offset);

ok( $node_A->poll_query_until(
		'postgres',
		"SELECT xmin IS NULL from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the xmin value of slot 'pg_conflict_detection' is invalid on Node A");

$result = $node_A->safe_psql('postgres',
	"SELECT subretentionactive FROM pg_subscription WHERE subname='$subname_AB';");
is($result, qq(f), 'retention is inactive');

###############################################################################
# Check that dead tuple retention resumes when the max_retention_duration is set
# 0.
###############################################################################

$log_offset = -s $node_A->logfile;

# Set max_retention_duration to 0
$node_A->safe_psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (max_retention_duration = 0);");

# Drop the physical slot and reset the synchronized_standby_slots setting. We
# change this after setting max_retention_duration to 0, ensuring consistent
# results in the test as the resumption becomes possible immediately after
# resetting synchronized_standby_slots, due to the smaller max_retention_duration
# value of 1ms.
$node_B->safe_psql('postgres',
	"SELECT * FROM pg_drop_replication_slot('blocker');");
$node_B->adjust_conf('postgresql.conf', 'synchronized_standby_slots', "''");
$node_B->reload;

# Confirm that the retention resumes
$node_A->wait_for_log(
	qr/logical replication worker for subscription "tap_sub_a_b" will resume retaining the information for detecting conflicts
.*DETAIL:.* Retention is re-enabled because max_retention_duration has been set to unlimited.*/,
	$log_offset);

ok( $node_A->poll_query_until(
		'postgres',
		"SELECT xmin IS NOT NULL from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the xmin value of slot 'pg_conflict_detection' is valid on Node A");

$result = $node_A->safe_psql('postgres',
	"SELECT subretentionactive FROM pg_subscription WHERE subname='$subname_AB';");
is($result, qq(t), 'retention is active');

###############################################################################
# Check that the replication slot pg_conflict_detection is dropped after
# removing all the subscriptions.
###############################################################################

$node_B->safe_psql(
	'postgres', "DROP SUBSCRIPTION $subname_BA");

ok( $node_B->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the slot 'pg_conflict_detection' has been dropped on Node B");

$node_A->safe_psql(
	'postgres', "DROP SUBSCRIPTION $subname_AB");

ok( $node_A->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the slot 'pg_conflict_detection' has been dropped on Node A");

done_testing();
