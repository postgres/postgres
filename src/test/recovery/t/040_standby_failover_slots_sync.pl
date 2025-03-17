
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

##################################################
# Test that when a subscription with failover enabled is created, it will alter
# the failover property of the corresponding slot on the publisher.
##################################################

# Create publisher
my $publisher = PostgreSQL::Test::Cluster->new('publisher');
# Make sure pg_hba.conf is set up to allow connections from repl_role.
# This is only needed on Windows machines that don't use UNIX sockets.
$publisher->init(
	allows_streaming => 'logical',
	auth_extra => [ '--create-role' => 'repl_role' ]);
# Disable autovacuum to avoid generating xid during stats update as otherwise
# the new XID could then be replicated to standby at some random point making
# slots at primary lag behind standby during slot sync.
$publisher->append_conf('postgresql.conf', 'autovacuum = off');
$publisher->start;

$publisher->safe_psql('postgres',
	"CREATE PUBLICATION regress_mypub FOR ALL TABLES;");

my $publisher_connstr = $publisher->connstr . ' dbname=postgres';

# Create a subscriber node, wait for sync to complete
my $subscriber1 = PostgreSQL::Test::Cluster->new('subscriber1');
$subscriber1->init;
$subscriber1->start;

# Capture the time before the logical failover slot is created on the
# primary. We later call this publisher as primary anyway.
my $slot_creation_time_on_primary = $publisher->safe_psql(
	'postgres', qq[
    SELECT current_timestamp;
]);

# Create a subscription that enables failover.
$subscriber1->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_mysub1 CONNECTION '$publisher_connstr' PUBLICATION regress_mypub WITH (slot_name = lsub1_slot, copy_data = false, failover = true, enabled = false);"
);

# Confirm that the failover flag on the slot is turned on
is( $publisher->safe_psql(
		'postgres',
		q{SELECT failover from pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"t",
	'logical slot has failover true on the publisher');

##################################################
# Test that changing the failover property of a subscription updates the
# corresponding failover property of the slot.
##################################################

# Disable failover
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 SET (failover = false)");

# Confirm that the failover flag on the slot has now been turned off
is( $publisher->safe_psql(
		'postgres',
		q{SELECT failover from pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"f",
	'logical slot has failover false on the publisher');

# Enable failover
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 SET (failover = true)");

# Confirm that the failover flag on the slot has now been turned on
is( $publisher->safe_psql(
		'postgres',
		q{SELECT failover from pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"t",
	'logical slot has failover true on the publisher');

##################################################
# Test that the failover option cannot be changed for enabled subscriptions.
##################################################

# Enable subscription
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 ENABLE");

# Disable failover for enabled subscription
my ($result, $stdout, $stderr) = $subscriber1->psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 SET (failover = false)");
ok( $stderr =~
	  /ERROR:  cannot set option "failover" for enabled subscription/,
	"altering failover is not allowed for enabled subscription");

##################################################
# Test that pg_sync_replication_slots() cannot be executed on a non-standby server.
##################################################

($result, $stdout, $stderr) =
  $publisher->psql('postgres', "SELECT pg_sync_replication_slots();");
ok( $stderr =~
	  /ERROR:  replication slots can only be synchronized to a standby server/,
	"cannot sync slots on a non-standby server");

##################################################
# Test logical failover slots corresponding to different plugins can be
# synced to the standby.
#
# Configure standby1 to replicate and synchronize logical slots configured
# for failover on the primary
#
#              failover slot lsub1_slot   |       output_plugin: pgoutput
#              failover slot lsub2_slot   |       output_plugin: test_decoding
# primary --->                            |
#              physical slot sb1_slot --->| ----> standby1 (connected via streaming replication)
#                                         |                 lsub1_slot, lsub2_slot (synced_slot)
##################################################

my $primary = $publisher;
my $backup_name = 'backup';
$primary->backup($backup_name);

# Create a standby
my $standby1 = PostgreSQL::Test::Cluster->new('standby1');
$standby1->init_from_backup(
	$primary, $backup_name,
	has_streaming => 1,
	has_restoring => 1);

# Increase the log_min_messages setting to DEBUG2 on both the standby and
# primary to debug test failures, if any.
my $connstr_1 = $primary->connstr;
$standby1->append_conf(
	'postgresql.conf', qq(
hot_standby_feedback = on
primary_slot_name = 'sb1_slot'
primary_conninfo = '$connstr_1 dbname=postgres'
log_min_messages = 'debug2'
));

$primary->append_conf('postgresql.conf', "log_min_messages = 'debug2'");
$primary->reload;

# Drop the subscription to prevent further advancement of the restart_lsn for
# the lsub1_slot.
$subscriber1->safe_psql('postgres', "DROP SUBSCRIPTION regress_mysub1;");

# To ensure that restart_lsn has moved to a recent WAL position, we re-create
# the lsub1_slot.
$primary->psql('postgres',
	q{SELECT pg_create_logical_replication_slot('lsub1_slot', 'pgoutput', false, false, true);}
);

$primary->psql('postgres',
	q{SELECT pg_create_logical_replication_slot('lsub2_slot', 'test_decoding', false, false, true);}
);

$primary->psql('postgres',
	q{SELECT pg_create_physical_replication_slot('sb1_slot');});

# Start the standby so that slot syncing can begin
$standby1->start;

# Capture the inactive_since of the slot from the primary. Note that the slot
# will be inactive since the corresponding subscription was dropped.
my $inactive_since_on_primary =
  $primary->validate_slot_inactive_since('lsub1_slot',
	$slot_creation_time_on_primary);

# Wait for the standby to catch up so that the standby is not lagging behind
# the failover slots.
$primary->wait_for_replay_catchup($standby1);

# Synchronize the primary server slots to the standby.
$standby1->safe_psql('postgres', "SELECT pg_sync_replication_slots();");

# Confirm that the logical failover slots are created on the standby and are
# flagged as 'synced'
is( $standby1->safe_psql(
		'postgres',
		q{SELECT count(*) = 2 FROM pg_replication_slots WHERE slot_name IN ('lsub1_slot', 'lsub2_slot') AND synced AND NOT temporary;}
	),
	"t",
	'logical slots have synced as true on standby');

# Capture the inactive_since of the synced slot on the standby
my $inactive_since_on_standby =
  $standby1->validate_slot_inactive_since('lsub1_slot',
	$slot_creation_time_on_primary);

# Synced slot on the standby must get its own inactive_since
is( $standby1->safe_psql(
		'postgres',
		"SELECT '$inactive_since_on_primary'::timestamptz < '$inactive_since_on_standby'::timestamptz;"
	),
	"t",
	'synchronized slot has got its own inactive_since');

##################################################
# Test that the synchronized slot will be dropped if the corresponding remote
# slot on the primary server has been dropped.
##################################################

$primary->psql('postgres', "SELECT pg_drop_replication_slot('lsub2_slot');");

$standby1->safe_psql('postgres', "SELECT pg_sync_replication_slots();");

is( $standby1->safe_psql(
		'postgres',
		q{SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = 'lsub2_slot';}
	),
	"t",
	'synchronized slot has been dropped');

##################################################
# Test that if the synchronized slot is invalidated while the remote slot is
# still valid, the slot will be dropped and re-created on the standby by
# executing pg_sync_replication_slots() again.
##################################################

# Configure the max_slot_wal_keep_size so that the synced slot can be
# invalidated due to wal removal.
$standby1->append_conf('postgresql.conf', 'max_slot_wal_keep_size = 64kB');
$standby1->reload;

# Generate some activity and switch WAL file on the primary
$primary->advance_wal(1);
$primary->psql('postgres', "CHECKPOINT");
$primary->wait_for_replay_catchup($standby1);

# Request a checkpoint on the standby to trigger the WAL file(s) removal
$standby1->safe_psql('postgres', "CHECKPOINT");

# Check if the synced slot is invalidated
is( $standby1->safe_psql(
		'postgres',
		q{SELECT invalidation_reason = 'wal_removed' FROM pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"t",
	'synchronized slot has been invalidated');

# Reset max_slot_wal_keep_size to avoid further wal removal
$standby1->append_conf('postgresql.conf', 'max_slot_wal_keep_size = -1');
$standby1->reload;

# Capture the time before the logical failover slot is created on the primary.
$slot_creation_time_on_primary = $publisher->safe_psql(
	'postgres', qq[
    SELECT current_timestamp;
]);

# To ensure that restart_lsn has moved to a recent WAL position, we re-create
# the lsub1_slot.
$primary->safe_psql(
	'postgres', qq[
	SELECT pg_drop_replication_slot('lsub1_slot');
	SELECT pg_create_logical_replication_slot('lsub1_slot', 'pgoutput', false, false, true);
]);

# Capture the inactive_since of the slot from the primary. Note that the slot
# will be inactive since the corresponding subscription was dropped.
$inactive_since_on_primary =
  $primary->validate_slot_inactive_since('lsub1_slot',
	$slot_creation_time_on_primary);

# Wait for the standby to catch up so that the standby is not lagging behind
# the failover slots.
$primary->wait_for_replay_catchup($standby1);

my $log_offset = -s $standby1->logfile;

# Synchronize the primary server slots to the standby.
$standby1->safe_psql('postgres', "SELECT pg_sync_replication_slots();");

# Confirm that the invalidated slot has been dropped.
$standby1->wait_for_log(
	qr/dropped replication slot "lsub1_slot" of database with OID [0-9]+/,
	$log_offset);

# Confirm that the logical slot has been re-created on the standby and is
# flagged as 'synced'
is( $standby1->safe_psql(
		'postgres',
		q{SELECT invalidation_reason IS NULL AND synced AND NOT temporary FROM pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"t",
	'logical slot is re-synced');

# Reset the log_min_messages to the default value.
$primary->append_conf('postgresql.conf', "log_min_messages = 'warning'");
$primary->reload;

$standby1->append_conf('postgresql.conf', "log_min_messages = 'warning'");
$standby1->reload;

##################################################
# Test that a synchronized slot can not be decoded, altered or dropped by the
# user
##################################################

# Attempting to perform logical decoding on a synced slot should result in an error
($result, $stdout, $stderr) = $standby1->psql('postgres',
	"select * from pg_logical_slot_get_changes('lsub1_slot', NULL, NULL);");
ok( $stderr =~
	  /ERROR:  cannot use replication slot "lsub1_slot" for logical decoding/,
	"logical decoding is not allowed on synced slot");

# Attempting to alter a synced slot should result in an error
($result, $stdout, $stderr) = $standby1->psql(
	'postgres',
	qq[ALTER_REPLICATION_SLOT lsub1_slot (failover);],
	replication => 'database');
ok($stderr =~ /ERROR:  cannot alter replication slot "lsub1_slot"/,
	"synced slot on standby cannot be altered");

# Attempting to drop a synced slot should result in an error
($result, $stdout, $stderr) = $standby1->psql('postgres',
	"SELECT pg_drop_replication_slot('lsub1_slot');");
ok($stderr =~ /ERROR:  cannot drop replication slot "lsub1_slot"/,
	"synced slot on standby cannot be dropped");

##################################################
# Test that we cannot synchronize slots if dbname is not specified in the
# primary_conninfo.
##################################################

$standby1->append_conf('postgresql.conf', "primary_conninfo = '$connstr_1'");
$standby1->reload;

($result, $stdout, $stderr) =
  $standby1->psql('postgres', "SELECT pg_sync_replication_slots();");
ok( $stderr =~
	  /ERROR:  replication slot synchronization requires "dbname" to be specified in "primary_conninfo"/,
	"cannot sync slots if dbname is not specified in primary_conninfo");

# Add the dbname back to the primary_conninfo for further tests
$standby1->append_conf('postgresql.conf',
	"primary_conninfo = '$connstr_1 dbname=postgres'");
$standby1->reload;

##################################################
# Test that we cannot synchronize slots to a cascading standby server.
##################################################

# Create a cascading standby
$backup_name = 'backup2';
$standby1->backup($backup_name);

my $cascading_standby = PostgreSQL::Test::Cluster->new('cascading_standby');
$cascading_standby->init_from_backup(
	$standby1, $backup_name,
	has_streaming => 1,
	has_restoring => 1);

my $cascading_connstr = $standby1->connstr;
$cascading_standby->append_conf(
	'postgresql.conf', qq(
hot_standby_feedback = on
primary_slot_name = 'cascading_sb_slot'
primary_conninfo = '$cascading_connstr dbname=postgres'
));

$standby1->psql('postgres',
	q{SELECT pg_create_physical_replication_slot('cascading_sb_slot');});

$cascading_standby->start;

($result, $stdout, $stderr) =
  $cascading_standby->psql('postgres', "SELECT pg_sync_replication_slots();");
ok( $stderr =~
	  /ERROR:  cannot synchronize replication slots from a standby server/,
	"cannot sync slots to a cascading standby server");

$cascading_standby->stop;

##################################################
# Create a failover slot and advance the restart_lsn to a position where a
# running transaction exists. This setup is for testing that the synced slots
# can achieve the consistent snapshot state starting from the restart_lsn
# after promotion without losing any data that otherwise would have been
# received from the primary.
##################################################

$primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('snap_test_slot', 'test_decoding', false, false, true);"
);

# Wait for the standby to catch up so that the standby is not lagging behind
# the failover slots.
$primary->wait_for_replay_catchup($standby1);

$standby1->safe_psql('postgres', "SELECT pg_sync_replication_slots();");

# Two xl_running_xacts logs are generated here. When decoding the first log, it
# only serializes the snapshot, without advancing the restart_lsn to the latest
# position. This is because if a transaction is running, the restart_lsn can
# only move to a position before that transaction. Hence, the second
# xl_running_xacts log is needed, the decoding for which allows the restart_lsn
# to advance to the last serialized snapshot's position (the first log).
$primary->safe_psql(
	'postgres', qq(
		BEGIN;
		SELECT txid_current();
		SELECT pg_log_standby_snapshot();
		COMMIT;
		BEGIN;
		SELECT txid_current();
		SELECT pg_log_standby_snapshot();
		COMMIT;
));

# Advance the restart_lsn to the position of the first xl_running_xacts log
# generated above. Note that there might be concurrent xl_running_xacts logs
# written by the bgwriter, which could cause the position to be advanced to an
# unexpected point, but that would be a rare scenario and doesn't affect the
# test results.
$primary->safe_psql('postgres',
	"SELECT pg_replication_slot_advance('snap_test_slot', pg_current_wal_lsn());"
);

# Wait for the standby to catch up so that the standby is not lagging behind
# the failover slots.
$primary->wait_for_replay_catchup($standby1);

# Log a message that will be consumed on the standby after promotion using the
# synced slot. See the test where we promote standby (Promote the standby1 to
# primary.)
$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(false, 'test', 'test');");

# Get the confirmed_flush_lsn for the logical slot snap_test_slot on the primary
my $confirmed_flush_lsn = $primary->safe_psql('postgres',
	"SELECT confirmed_flush_lsn from pg_replication_slots WHERE slot_name = 'snap_test_slot';"
);

$standby1->safe_psql('postgres', "SELECT pg_sync_replication_slots();");

# Verify that confirmed_flush_lsn of snap_test_slot slot is synced to the standby
ok( $standby1->poll_query_until(
		'postgres',
		"SELECT '$confirmed_flush_lsn' = confirmed_flush_lsn from pg_replication_slots WHERE slot_name = 'snap_test_slot' AND synced AND NOT temporary;"
	),
	'confirmed_flush_lsn of slot snap_test_slot synced to standby');

##################################################
# Test to confirm that the slot synchronization is protected from malicious
# users.
##################################################

$primary->psql('postgres', "CREATE DATABASE slotsync_test_db");
$primary->wait_for_replay_catchup($standby1);

$standby1->stop;

# On the primary server, create '=' operator in another schema mapped to
# inequality function and redirect the queries to use new operator by setting
# search_path. The new '=' operator is created with leftarg as 'bigint' and
# right arg as 'int' to redirect 'count(*) = 1' in slot sync's query to use
# new '=' operator.
$primary->safe_psql(
	'slotsync_test_db', q{

CREATE ROLE repl_role REPLICATION LOGIN;
CREATE SCHEMA myschema;

CREATE FUNCTION myschema.myintne(bigint, int) RETURNS bool as $$
		BEGIN
		  RETURN $1 <> $2;
		END;
	  $$ LANGUAGE plpgsql immutable;

CREATE OPERATOR myschema.= (
	  leftarg    = bigint,
	  rightarg   = int,
	  procedure  = myschema.myintne);

ALTER DATABASE slotsync_test_db SET SEARCH_PATH TO myschema,pg_catalog;
GRANT USAGE on SCHEMA myschema TO repl_role;
});

# Start the standby with changed primary_conninfo.
$standby1->append_conf('postgresql.conf',
	"primary_conninfo = '$connstr_1 dbname=slotsync_test_db user=repl_role'");
$standby1->start;

# Run the synchronization function. If the sync flow was not prepared
# to handle such attacks, it would have failed during the validation
# of the primary_slot_name itself resulting in
# ERROR:  slot synchronization requires valid primary_slot_name
$standby1->safe_psql('slotsync_test_db',
	"SELECT pg_sync_replication_slots();");

# Reset the dbname and user in primary_conninfo to the earlier values.
$standby1->append_conf('postgresql.conf',
	"primary_conninfo = '$connstr_1 dbname=postgres'");
$standby1->reload;

# Drop the newly created database.
$primary->psql('postgres', q{DROP DATABASE slotsync_test_db;});

##################################################
# Test to confirm that the slot sync worker exits on invalid GUC(s) and
# get started again on valid GUC(s).
##################################################

$log_offset = -s $standby1->logfile;

# Enable slot sync worker.
$standby1->append_conf('postgresql.conf', qq(sync_replication_slots = on));
$standby1->reload;

# Confirm that the slot sync worker is able to start.
$standby1->wait_for_log(qr/slot sync worker started/, $log_offset);

$log_offset = -s $standby1->logfile;

# Disable another GUC required for slot sync.
$standby1->append_conf('postgresql.conf', qq(hot_standby_feedback = off));
$standby1->reload;

# Confirm that slot sync worker acknowledge the GUC change and logs the msg
# about wrong configuration.
$standby1->wait_for_log(
	qr/slot synchronization worker will restart because of a parameter change/,
	$log_offset);
$standby1->wait_for_log(
	qr/slot synchronization requires "hot_standby_feedback" to be enabled/,
	$log_offset);

$log_offset = -s $standby1->logfile;

# Re-enable the required GUC
$standby1->append_conf('postgresql.conf', "hot_standby_feedback = on");
$standby1->reload;

# Confirm that the slot sync worker is able to start now.
$standby1->wait_for_log(qr/slot sync worker started/, $log_offset);

##################################################
# Test to confirm that confirmed_flush_lsn of the logical slot on the primary
# is synced to the standby via the slot sync worker.
##################################################

# Insert data on the primary
$primary->safe_psql(
	'postgres', qq[
	CREATE TABLE tab_int (a int PRIMARY KEY);
	INSERT INTO tab_int SELECT generate_series(1, 10);
]);

# Subscribe to the new table data and wait for it to arrive
$subscriber1->safe_psql(
	'postgres', qq[
	CREATE TABLE tab_int (a int PRIMARY KEY);
	CREATE SUBSCRIPTION regress_mysub1 CONNECTION '$publisher_connstr' PUBLICATION regress_mypub WITH (slot_name = lsub1_slot, failover = true, create_slot = false);
]);

$subscriber1->wait_for_subscription_sync;

# Do not allow any further advancement of the confirmed_flush_lsn for the
# lsub1_slot.
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 DISABLE");

# Wait for the replication slot to become inactive on the publisher
$primary->poll_query_until(
	'postgres',
	"SELECT COUNT(*) FROM pg_catalog.pg_replication_slots WHERE slot_name = 'lsub1_slot' AND active='f'",
	1);

# Get the confirmed_flush_lsn for the logical slot lsub1_slot on the primary
my $primary_flush_lsn = $primary->safe_psql('postgres',
	"SELECT confirmed_flush_lsn from pg_replication_slots WHERE slot_name = 'lsub1_slot';"
);

# Confirm that confirmed_flush_lsn of lsub1_slot slot is synced to the standby
ok( $standby1->poll_query_until(
		'postgres',
		"SELECT '$primary_flush_lsn' = confirmed_flush_lsn from pg_replication_slots WHERE slot_name = 'lsub1_slot' AND synced AND NOT temporary;"
	),
	'confirmed_flush_lsn of slot lsub1_slot synced to standby');

##################################################
# Test that logical failover replication slots wait for the specified
# physical replication slots to receive the changes first. It uses the
# following set up:
#
#				(physical standbys)
#				| ----> standby1 (primary_slot_name = sb1_slot)
#				| ----> standby2 (primary_slot_name = sb2_slot)
# primary -----	|
#				(logical replication)
#				| ----> subscriber1 (failover = true, slot_name = lsub1_slot)
#				| ----> subscriber2 (failover = false, slot_name = lsub2_slot)
#
# synchronized_standby_slots = 'sb1_slot'
#
# The setup is configured in such a way that the logical slot of subscriber1 is
# enabled for failover, and thus the subscriber1 will wait for the physical
# slot of standby1(sb1_slot) to catch up before receiving the decoded changes.
##################################################

$backup_name = 'backup3';

$primary->psql('postgres',
	q{SELECT pg_create_physical_replication_slot('sb2_slot');});

$primary->backup($backup_name);

# Create another standby
my $standby2 = PostgreSQL::Test::Cluster->new('standby2');
$standby2->init_from_backup(
	$primary, $backup_name,
	has_streaming => 1,
	has_restoring => 1);
$standby2->append_conf(
	'postgresql.conf', qq(
primary_slot_name = 'sb2_slot'
));
$standby2->start;
$primary->wait_for_replay_catchup($standby2);

# Configure primary to disallow any logical slots that have enabled failover
# from getting ahead of the specified physical replication slot (sb1_slot).
$primary->append_conf(
	'postgresql.conf', qq(
synchronized_standby_slots = 'sb1_slot'
));
$primary->reload;

# Create another subscriber node without enabling failover, wait for sync to
# complete
my $subscriber2 = PostgreSQL::Test::Cluster->new('subscriber2');
$subscriber2->init;
$subscriber2->start;
$subscriber2->safe_psql(
	'postgres', qq[
	CREATE TABLE tab_int (a int PRIMARY KEY);
	CREATE SUBSCRIPTION regress_mysub2 CONNECTION '$publisher_connstr' PUBLICATION regress_mypub WITH (slot_name = lsub2_slot);
]);

$subscriber2->wait_for_subscription_sync;

$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 ENABLE");

my $offset = -s $primary->logfile;

# Stop the standby associated with the specified physical replication slot
# (sb1_slot) so that the logical replication slot (lsub1_slot) won't receive
# changes until the standby comes up.
$standby1->stop;

# Create some data on the primary
my $primary_row_count = 20;
$primary->safe_psql('postgres',
	"INSERT INTO tab_int SELECT generate_series(11, $primary_row_count);");

# Wait until the standby2 that's still running gets the data from the primary
$primary->wait_for_replay_catchup($standby2);
$result = $standby2->safe_psql('postgres',
	"SELECT count(*) = $primary_row_count FROM tab_int;");
is($result, 't', "standby2 gets data from primary");

# Wait for regress_mysub2 to get the data from the primary. This subscription
# was not enabled for failover so it gets the data without waiting for any
# standbys.
$primary->wait_for_catchup('regress_mysub2');
$result = $subscriber2->safe_psql('postgres',
	"SELECT count(*) = $primary_row_count FROM tab_int;");
is($result, 't', "subscriber2 gets data from primary");

# Wait until the primary server logs a warning indicating that it is waiting
# for the sb1_slot to catch up.
$primary->wait_for_log(
	qr/replication slot \"sb1_slot\" specified in parameter "synchronized_standby_slots" does not have active_pid/,
	$offset);

# The regress_mysub1 was enabled for failover so it doesn't get the data from
# primary and keeps waiting for the standby specified in synchronized_standby_slots
# (sb1_slot aka standby1).
$result =
  $subscriber1->safe_psql('postgres',
	"SELECT count(*) <> $primary_row_count FROM tab_int;");
is($result, 't',
	"subscriber1 doesn't get data from primary until standby1 acknowledges changes"
);

# Start the standby specified in synchronized_standby_slots (sb1_slot aka standby1) and
# wait for it to catch up with the primary.
$standby1->start;
$primary->wait_for_replay_catchup($standby1);
$result = $standby1->safe_psql('postgres',
	"SELECT count(*) = $primary_row_count FROM tab_int;");
is($result, 't', "standby1 gets data from primary");

# Now that the standby specified in synchronized_standby_slots is up and running, the
# primary can send the decoded changes to the subscription enabled for failover
# (i.e. regress_mysub1). While the standby was down, regress_mysub1 didn't
# receive any data from the primary. i.e. the primary didn't allow it to go
# ahead of standby.
$primary->wait_for_catchup('regress_mysub1');
$result = $subscriber1->safe_psql('postgres',
	"SELECT count(*) = $primary_row_count FROM tab_int;");
is($result, 't',
	"subscriber1 gets data from primary after standby1 acknowledges changes");

##################################################
# Verify that when using pg_logical_slot_get_changes to consume changes from a
# logical failover slot, it will also wait for the slots specified in
# synchronized_standby_slots to catch up.
##################################################

# Stop the standby associated with the specified physical replication slot so
# that the logical replication slot won't receive changes until the standby
# slot's restart_lsn is advanced or the slot is removed from the
# synchronized_standby_slots list.
$primary->safe_psql('postgres', "TRUNCATE tab_int;");
$primary->wait_for_catchup('regress_mysub1');
$standby1->stop;

# Disable the regress_mysub1 to prevent the logical walsender from generating
# more warnings.
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 DISABLE");

# Wait for the replication slot to become inactive on the publisher
$primary->poll_query_until(
	'postgres',
	"SELECT COUNT(*) FROM pg_catalog.pg_replication_slots WHERE slot_name = 'lsub1_slot' AND active = 'f'",
	1);

# Create a logical 'test_decoding' replication slot with failover enabled
$primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('test_slot', 'test_decoding', false, false, true);"
);

my $back_q = $primary->background_psql(
	'postgres',
	on_error_stop => 0,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

# pg_logical_slot_get_changes will be blocked until the standby catches up,
# hence it needs to be executed in a background session.
$offset = -s $primary->logfile;
$back_q->query_until(
	qr/logical_slot_get_changes/, q(
   \echo logical_slot_get_changes
   SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);
));

# Wait until the primary server logs a warning indicating that it is waiting
# for the sb1_slot to catch up.
$primary->wait_for_log(
	qr/replication slot \"sb1_slot\" specified in parameter "synchronized_standby_slots" does not have active_pid/,
	$offset);

# Remove the standby from the synchronized_standby_slots list and reload the
# configuration.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots', "''");
$primary->reload;

# Since there are no slots in synchronized_standby_slots, the function
# pg_logical_slot_get_changes should now return, and the session can be
# stopped.
$back_q->quit;

$primary->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('test_slot');");

# Add the physical slot (sb1_slot) back to the synchronized_standby_slots for further
# tests.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'sb1_slot'");
$primary->reload;

# Enable the regress_mysub1 for further tests
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 ENABLE");

##################################################
# Test that logical replication will wait for the user-created inactive
# physical slot to catch up until we remove the slot from synchronized_standby_slots.
##################################################

$offset = -s $primary->logfile;

# Create some data on the primary
$primary_row_count = 10;
$primary->safe_psql('postgres',
	"INSERT INTO tab_int SELECT generate_series(1, $primary_row_count);");

# Wait until the primary server logs a warning indicating that it is waiting
# for the sb1_slot to catch up.
$primary->wait_for_log(
	qr/replication slot \"sb1_slot\" specified in parameter "synchronized_standby_slots" does not have active_pid/,
	$offset);

# The regress_mysub1 doesn't get the data from primary because the specified
# standby slot (sb1_slot) in synchronized_standby_slots is inactive.
$result =
  $subscriber1->safe_psql('postgres', "SELECT count(*) = 0 FROM tab_int;");
is($result, 't',
	"subscriber1 doesn't get data as the sb1_slot doesn't catch up");

# Remove the standby from the synchronized_standby_slots list and reload the
# configuration.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots', "''");
$primary->reload;

# Since there are no slots in synchronized_standby_slots, the primary server should now
# send the decoded changes to the subscription.
$primary->wait_for_catchup('regress_mysub1');
$result = $subscriber1->safe_psql('postgres',
	"SELECT count(*) = $primary_row_count FROM tab_int;");
is($result, 't',
	"subscriber1 gets data from primary after standby1 is removed from the synchronized_standby_slots list"
);

# Add the physical slot (sb1_slot) back to the synchronized_standby_slots for further
# tests.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'sb1_slot'");
$primary->reload;

##################################################
# Promote the standby1 to primary. Confirm that:
# a) the slot 'lsub1_slot' and 'snap_test_slot' are retained on the new primary
# b) logical replication for regress_mysub1 is resumed successfully after failover
# c) changes can be consumed from the synced slot 'snap_test_slot'
##################################################
$standby1->start;
$primary->wait_for_replay_catchup($standby1);

# Capture the time before the standby is promoted
my $promotion_time_on_primary = $standby1->safe_psql(
	'postgres', qq[
    SELECT current_timestamp;
]);

$standby1->promote;

# Capture the inactive_since of the synced slot after the promotion.
# The expectation here is that the slot gets its inactive_since as part of the
# promotion. We do this check before the slot is enabled on the new primary
# below, otherwise, the slot gets active setting inactive_since to NULL.
my $inactive_since_on_new_primary =
  $standby1->validate_slot_inactive_since('lsub1_slot',
	$promotion_time_on_primary);

is( $standby1->safe_psql(
		'postgres',
		"SELECT '$inactive_since_on_new_primary'::timestamptz > '$inactive_since_on_primary'::timestamptz"
	),
	"t",
	'synchronized slot has got its own inactive_since on the new primary after promotion'
);

# Update subscription with the new primary's connection info
my $standby1_conninfo = $standby1->connstr . ' dbname=postgres';
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 CONNECTION '$standby1_conninfo';");

# Confirm the synced slot 'lsub1_slot' is retained on the new primary
is( $standby1->safe_psql(
		'postgres',
		q{SELECT count(*) = 2 FROM pg_replication_slots WHERE slot_name IN ('lsub1_slot', 'snap_test_slot') AND synced AND NOT temporary;}
	),
	't',
	'synced slot retained on the new primary');

# Insert data on the new primary
$standby1->safe_psql('postgres',
	"INSERT INTO tab_int SELECT generate_series(11, 20);");
$standby1->wait_for_catchup('regress_mysub1');

# Confirm that data in tab_int replicated on the subscriber
is($subscriber1->safe_psql('postgres', q{SELECT count(*) FROM tab_int;}),
	"20", 'data replicated from the new primary');

# Consume the data from the snap_test_slot. The synced slot should reach a
# consistent point by restoring the snapshot at the restart_lsn serialized
# during slot synchronization.
$result = $standby1->safe_psql('postgres',
	"SELECT count(*) FROM pg_logical_slot_get_changes('snap_test_slot', NULL, NULL) WHERE data ~ 'message*';"
);

is($result, '1', "data can be consumed using snap_test_slot");

done_testing();
