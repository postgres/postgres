# Copyright (c) 2023-2024, PostgreSQL Global Development Group

# Tests for upgrading logical replication slots

use strict;
use warnings FATAL => 'all';

use File::Find qw(find);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Initialize old cluster
my $oldpub = PostgreSQL::Test::Cluster->new('oldpub');
$oldpub->init(allows_streaming => 'logical');
$oldpub->append_conf('postgresql.conf', 'autovacuum = off');

# Initialize new cluster
my $newpub = PostgreSQL::Test::Cluster->new('newpub');
$newpub->init(allows_streaming => 'logical');

# During upgrade, when pg_restore performs CREATE DATABASE, bgwriter or
# checkpointer may flush buffers and hold a file handle for the system table.
# So, if later due to some reason we need to re-create the file with the same
# name like a TRUNCATE command on the same table, then the command will fail
# if OS (such as older Windows versions) doesn't remove an unlinked file
# completely till it is open. The probability of seeing this behavior is
# higher in this test because we use wal_level as logical via
# allows_streaming => 'logical' which in turn set shared_buffers as 1MB.
$newpub->append_conf(
	'postgresql.conf', q{
bgwriter_lru_maxpages = 0
checkpoint_timeout = 1h
});

# Setup a common pg_upgrade command to be used by all the test cases
my @pg_upgrade_cmd = (
	'pg_upgrade', '--no-sync',
	'-d', $oldpub->data_dir,
	'-D', $newpub->data_dir,
	'-b', $oldpub->config_data('--bindir'),
	'-B', $newpub->config_data('--bindir'),
	'-s', $newpub->host,
	'-p', $oldpub->port,
	'-P', $newpub->port,
	$mode);

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

# ------------------------------
# TEST: Confirm pg_upgrade fails when the new cluster has wrong GUC values

# Preparations for the subsequent test:
# 1. Create two slots on the old cluster
$oldpub->start;
$oldpub->safe_psql(
	'postgres', qq[
	SELECT pg_create_logical_replication_slot('test_slot1', 'test_decoding');
	SELECT pg_create_logical_replication_slot('test_slot2', 'test_decoding');
]);
$oldpub->stop();

# 2. Set 'max_replication_slots' to be less than the number of slots (2)
#	 present on the old cluster.
$newpub->append_conf('postgresql.conf', "max_replication_slots = 1");

# pg_upgrade will fail because the new cluster has insufficient
# max_replication_slots
command_checks_all(
	[@pg_upgrade_cmd],
	1,
	[
		qr/"max_replication_slots" \(1\) must be greater than or equal to the number of logical replication slots \(2\) on the old cluster/
	],
	[qr//],
	'run of pg_upgrade where the new cluster has insufficient "max_replication_slots"'
);
ok(-d $newpub->data_dir . "/pg_upgrade_output.d",
	"pg_upgrade_output.d/ not removed after pg_upgrade failure");

# Set 'max_replication_slots' to match the number of slots (2) present on the
# old cluster. Both slots will be used for subsequent tests.
$newpub->append_conf('postgresql.conf', "max_replication_slots = 2");


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
$oldpub->start;
$oldpub->safe_psql(
	'postgres', qq[
		CREATE TABLE tbl AS SELECT generate_series(1, 10) AS a;
		SELECT pg_replication_slot_advance('test_slot2', pg_current_wal_lsn());
		SELECT count(*) FROM pg_logical_emit_message('false', 'prefix', 'This is a non-transactional message');
]);
$oldpub->stop;

# pg_upgrade will fail because there are slots still having unconsumed WAL
# records
command_checks_all(
	[@pg_upgrade_cmd],
	1,
	[
		qr/Your installation contains logical replication slots that cannot be upgraded./
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
		if ($File::Find::name =~ m/invalid_logical_slots\.txt/)
		{
			$slots_filename = $File::Find::name;
		}
	},
	$newpub->data_dir . "/pg_upgrade_output.d");

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
my $old_connstr = $oldpub->connstr . ' dbname=postgres';

$oldpub->start;
$oldpub->safe_psql(
	'postgres', qq[
	SELECT * FROM pg_drop_replication_slot('test_slot1');
	SELECT * FROM pg_drop_replication_slot('test_slot2');
	CREATE PUBLICATION regress_pub FOR ALL TABLES;
]);

# Initialize subscriber cluster
my $sub = PostgreSQL::Test::Cluster->new('sub');
$sub->init();

$sub->start;
$sub->safe_psql(
	'postgres', qq[
	CREATE TABLE tbl (a int);
	CREATE SUBSCRIPTION regress_sub CONNECTION '$old_connstr' PUBLICATION regress_pub WITH (two_phase = 'true', failover = 'true')
]);
$sub->wait_for_subscription_sync($oldpub, 'regress_sub');

# Also wait for two-phase to be enabled
my $twophase_query =
  "SELECT count(1) = 0 FROM pg_subscription WHERE subtwophasestate NOT IN ('e');";
$sub->poll_query_until('postgres', $twophase_query)
  or die "Timed out while waiting for subscriber to enable twophase";

# 2. Temporarily disable the subscription
$sub->safe_psql('postgres', "ALTER SUBSCRIPTION regress_sub DISABLE");
$oldpub->stop;

# pg_upgrade should be successful
command_ok([@pg_upgrade_cmd], 'run of pg_upgrade of old cluster');

# Check that the slot 'regress_sub' has migrated to the new cluster
$newpub->start;
my $result = $newpub->safe_psql('postgres',
	"SELECT slot_name, two_phase, failover FROM pg_replication_slots");
is($result, qq(regress_sub|t|t), 'check the slot exists on new cluster');

# Update the connection
my $new_connstr = $newpub->connstr . ' dbname=postgres';
$sub->safe_psql(
	'postgres', qq[
	ALTER SUBSCRIPTION regress_sub CONNECTION '$new_connstr';
	ALTER SUBSCRIPTION regress_sub ENABLE;
]);

# Check whether changes on the new publisher get replicated to the subscriber
$newpub->safe_psql('postgres',
	"INSERT INTO tbl VALUES (generate_series(11, 20))");
$newpub->wait_for_catchup('regress_sub');
$result = $sub->safe_psql('postgres', "SELECT count(*) FROM tbl");
is($result, qq(20), 'check changes are replicated to the sub');

# Clean up
$sub->stop();
$newpub->stop();

done_testing();
