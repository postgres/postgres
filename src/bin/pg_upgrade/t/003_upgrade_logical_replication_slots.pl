# Copyright (c) 2023, PostgreSQL Global Development Group

# Tests for upgrading logical replication slots

use strict;
use warnings;

use File::Find qw(find);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Initialize old cluster
my $old_publisher = PostgreSQL::Test::Cluster->new('old_publisher');
$old_publisher->init(allows_streaming => 'logical');

# Initialize new cluster
my $new_publisher = PostgreSQL::Test::Cluster->new('new_publisher');
$new_publisher->init(allows_streaming => 'logical');

# Setup a pg_upgrade command. This will be used anywhere.
my @pg_upgrade_cmd = (
	'pg_upgrade', '--no-sync',
	'-d', $old_publisher->data_dir,
	'-D', $new_publisher->data_dir,
	'-b', $old_publisher->config_data('--bindir'),
	'-B', $new_publisher->config_data('--bindir'),
	'-s', $new_publisher->host,
	'-p', $old_publisher->port,
	'-P', $new_publisher->port,
	$mode);

# ------------------------------
# TEST: Confirm pg_upgrade fails when the new cluster has wrong GUC values

# Preparations for the subsequent test:
# 1. Create two slots on the old cluster
$old_publisher->start;
$old_publisher->safe_psql(
	'postgres', qq[
	SELECT pg_create_logical_replication_slot('test_slot1', 'test_decoding');
	SELECT pg_create_logical_replication_slot('test_slot2', 'test_decoding');
]);
$old_publisher->stop();

# 2. Set 'max_replication_slots' to be less than the number of slots (2)
#	 present on the old cluster.
$new_publisher->append_conf('postgresql.conf', "max_replication_slots = 1");

# pg_upgrade will fail because the new cluster has insufficient
# max_replication_slots
command_checks_all(
	[@pg_upgrade_cmd],
	1,
	[
		qr/max_replication_slots \(1\) must be greater than or equal to the number of logical replication slots \(2\) on the old cluster/
	],
	[qr//],
	'run of pg_upgrade where the new cluster has insufficient max_replication_slots'
);
ok( -d $new_publisher->data_dir . "/pg_upgrade_output.d",
	"pg_upgrade_output.d/ not removed after pg_upgrade failure");

# Set 'max_replication_slots' to match the number of slots (2) present on the
# old cluster. Both slots will be used for subsequent tests.
$new_publisher->append_conf('postgresql.conf', "max_replication_slots = 2");


# ------------------------------
# TEST: Confirm pg_upgrade fails when the slot still has unconsumed WAL records

# Preparations for the subsequent test:
# 1. Generate extra WAL records. At this point neither test_slot1 nor
#	 test_slot2 has consumed them.
#
# 2. Advance the slot test_slot2 up to the current WAL location, but test_slot1
#	 still has unconsumed WAL records.
#
# 3. Emit a non-transactional message. This will cause test_slot2 to detect the
#	 unconsumed WAL record.
$old_publisher->start;
$old_publisher->safe_psql(
	'postgres', qq[
		CREATE TABLE tbl AS SELECT generate_series(1, 10) AS a;
		SELECT pg_replication_slot_advance('test_slot2', pg_current_wal_lsn());
		SELECT count(*) FROM pg_logical_emit_message('false', 'prefix', 'This is a non-transactional message');
]);
$old_publisher->stop;

# pg_upgrade will fail because there are slots still having unconsumed WAL
# records
command_checks_all(
	[@pg_upgrade_cmd],
	1,
	[
		qr/Your installation contains logical replication slots that can't be upgraded./
	],
	[qr//],
	'run of pg_upgrade of old cluster with slots having unconsumed WAL records'
);

# Verify the reason why the logical replication slot cannot be upgraded
my $slots_filename;

# Find a txt file that contains a list of logical replication slots that cannot
# be upgraded. We cannot predict the file's path because the output directory
# contains a milliseconds timestamp. File::Find::find must be used.
find(
	sub {
		if ($File::Find::name =~ m/invalid_logical_replication_slots\.txt/)
		{
			$slots_filename = $File::Find::name;
		}
	},
	$new_publisher->data_dir . "/pg_upgrade_output.d");

# Check the file content. Both slots should be reporting that they have
# unconsumed WAL records.
like(
	slurp_file($slots_filename),
	qr/The slot \"test_slot1\" has not consumed the WAL yet/m,
	'the previous test failed due to unconsumed WALs');
like(
	slurp_file($slots_filename),
	qr/The slot \"test_slot2\" has not consumed the WAL yet/m,
	'the previous test failed due to unconsumed WALs');


# ------------------------------
# TEST: Successful upgrade

# Preparations for the subsequent test:
# 1. Setup logical replication (first, cleanup slots from the previous tests)
my $old_connstr = $old_publisher->connstr . ' dbname=postgres';

$old_publisher->start;
$old_publisher->safe_psql(
	'postgres', qq[
	SELECT * FROM pg_drop_replication_slot('test_slot1');
	SELECT * FROM pg_drop_replication_slot('test_slot2');
	CREATE PUBLICATION regress_pub FOR ALL TABLES;
]);

# Initialize subscriber cluster
my $subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$subscriber->init();

$subscriber->start;
$subscriber->safe_psql(
	'postgres', qq[
	CREATE TABLE tbl (a int);
	CREATE SUBSCRIPTION regress_sub CONNECTION '$old_connstr' PUBLICATION regress_pub WITH (two_phase = 'true')
]);
$subscriber->wait_for_subscription_sync($old_publisher, 'regress_sub');

# 2. Temporarily disable the subscription
$subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION regress_sub DISABLE");
$old_publisher->stop;

# pg_upgrade should be successful
command_ok([@pg_upgrade_cmd], 'run of pg_upgrade of old cluster');

# Check that the slot 'regress_sub' has migrated to the new cluster
$new_publisher->start;
my $result = $new_publisher->safe_psql('postgres',
	"SELECT slot_name, two_phase FROM pg_replication_slots");
is($result, qq(regress_sub|t), 'check the slot exists on new cluster');

# Update the connection
my $new_connstr = $new_publisher->connstr . ' dbname=postgres';
$subscriber->safe_psql(
	'postgres', qq[
	ALTER SUBSCRIPTION regress_sub CONNECTION '$new_connstr';
	ALTER SUBSCRIPTION regress_sub ENABLE;
]);

# Check whether changes on the new publisher get replicated to the subscriber
$new_publisher->safe_psql('postgres',
	"INSERT INTO tbl VALUES (generate_series(11, 20))");
$new_publisher->wait_for_catchup('regress_sub');
$result = $subscriber->safe_psql('postgres', "SELECT count(*) FROM tbl");
is($result, qq(20), 'check changes are replicated to the subscriber');

# Clean up
$subscriber->stop();
$new_publisher->stop();

done_testing();
