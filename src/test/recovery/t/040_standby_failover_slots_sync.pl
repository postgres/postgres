
# Copyright (c) 2024, PostgreSQL Global Development Group

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

##################################################
# Test that when a subscription with failover enabled is created, it will alter
# the failover property of the corresponding slot on the publisher.
##################################################

# Create publisher
my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
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

# Create a slot on the publisher with failover disabled
$publisher->safe_psql('postgres',
	"SELECT 'init' FROM pg_create_logical_replication_slot('lsub1_slot', 'pgoutput', false, false, false);"
);

# Confirm that the failover flag on the slot is turned off
is( $publisher->safe_psql(
		'postgres',
		q{SELECT failover from pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"f",
	'logical slot has failover false on the publisher');

# Create a subscription (using the same slot created above) that enables
# failover.
$subscriber1->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_mysub1 CONNECTION '$publisher_connstr' PUBLICATION regress_mypub WITH (slot_name = lsub1_slot, copy_data=false, failover = true, create_slot = false, enabled = false);"
);

# Confirm that the failover flag on the slot has now been turned on
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
ok( $stderr =~ /ERROR:  cannot set failover for enabled subscription/,
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
# Test logical failover slots on the standby
# Configure standby1 to replicate and synchronize logical slots configured
# for failover on the primary
#
#              failover slot lsub1_slot ->| ----> subscriber1 (connected via logical replication)
#              failover slot lsub2_slot   |       inactive
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

$primary->psql('postgres',
	q{SELECT pg_create_logical_replication_slot('lsub2_slot', 'test_decoding', false, false, true);}
);

$primary->psql('postgres',
	q{SELECT pg_create_physical_replication_slot('sb1_slot');});

# Start the standby so that slot syncing can begin
$standby1->start;

$primary->wait_for_catchup('regress_mysub1');

# Do not allow any further advancement of the restart_lsn for the lsub1_slot.
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 DISABLE");

# Wait for the replication slot to become inactive on the publisher
$primary->poll_query_until(
	'postgres',
	"SELECT COUNT(*) FROM pg_catalog.pg_replication_slots WHERE slot_name = 'lsub1_slot' AND active = 'f'",
	1);

# Wait for the standby to catch up so that the standby is not lagging behind
# the subscriber.
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
		q{SELECT conflict_reason = 'wal_removed' FROM pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"t",
	'synchronized slot has been invalidated');

# Reset max_slot_wal_keep_size to avoid further wal removal
$standby1->append_conf('postgresql.conf', 'max_slot_wal_keep_size = -1');
$standby1->reload;

# To ensure that restart_lsn has moved to a recent WAL position, we re-create
# the subscription and the logical slot.
$subscriber1->safe_psql(
	'postgres', qq[
	DROP SUBSCRIPTION regress_mysub1;
	CREATE SUBSCRIPTION regress_mysub1 CONNECTION '$publisher_connstr' PUBLICATION regress_mypub WITH (slot_name = lsub1_slot, copy_data = false, failover = true);
]);

$primary->wait_for_catchup('regress_mysub1');

# Do not allow any further advancement of the restart_lsn for the lsub1_slot.
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 DISABLE");

# Wait for the replication slot to become inactive on the publisher
$primary->poll_query_until(
	'postgres',
	"SELECT COUNT(*) FROM pg_catalog.pg_replication_slots WHERE slot_name = 'lsub1_slot' AND active = 'f'",
	1);

# Wait for the standby to catch up so that the standby is not lagging behind
# the subscriber.
$primary->wait_for_replay_catchup($standby1);

my $log_offset = -s $standby1->logfile;

# Synchronize the primary server slots to the standby.
$standby1->safe_psql('postgres', "SELECT pg_sync_replication_slots();");

# Confirm that the invalidated slot has been dropped.
$standby1->wait_for_log(qr/dropped replication slot "lsub1_slot" of dbid [0-9]+/,
	$log_offset);

# Confirm that the logical slot has been re-created on the standby and is
# flagged as 'synced'
is( $standby1->safe_psql(
		'postgres',
		q{SELECT conflict_reason IS NULL AND synced AND NOT temporary FROM pg_replication_slots WHERE slot_name = 'lsub1_slot';}
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
	  /ERROR:  slot synchronization requires dbname to be specified in primary_conninfo/,
	"cannot sync slots if dbname is not specified in primary_conninfo");

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

done_testing();
