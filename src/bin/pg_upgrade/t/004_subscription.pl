# Copyright (c) 2023-2025, PostgreSQL Global Development Group

# Test for pg_upgrade of logical subscription. Note that after the successful
# upgrade test, we can't use the old cluster to prevent failing in --link mode.
use strict;
use warnings FATAL => 'all';

use File::Find qw(find);
use File::Path qw(rmtree);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Initialize publisher node
my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->start;

# Initialize the old subscriber node
my $old_sub = PostgreSQL::Test::Cluster->new('old_sub');
$old_sub->init;
$old_sub->start;
my $oldbindir = $old_sub->config_data('--bindir');

# Initialize the new subscriber
my $new_sub = PostgreSQL::Test::Cluster->new('new_sub');
$new_sub->init;
my $newbindir = $new_sub->config_data('--bindir');

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

# Remember a connection string for the publisher node. It would be used
# several times.
my $connstr = $publisher->connstr . ' dbname=postgres';

# ------------------------------------------------------
# Check that pg_upgrade fails when max_active_replication_origins configured
# in the new cluster is less than the number of subscriptions in the old
# cluster.
# ------------------------------------------------------
# It is sufficient to use disabled subscription to test upgrade failure.
$publisher->safe_psql('postgres', "CREATE PUBLICATION regress_pub1");
$old_sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_sub1 CONNECTION '$connstr' PUBLICATION regress_pub1 WITH (enabled = false)"
);

$old_sub->stop;

$new_sub->append_conf('postgresql.conf',
	"max_active_replication_origins = 0");

# pg_upgrade will fail because the new cluster has insufficient
# max_active_replication_origins.
command_checks_all(
	[
		'pg_upgrade',
		'--no-sync',
		'--old-datadir' => $old_sub->data_dir,
		'--new-datadir' => $new_sub->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $new_sub->host,
		'--old-port' => $old_sub->port,
		'--new-port' => $new_sub->port,
		$mode,
		'--check',
	],
	1,
	[
		qr/"max_active_replication_origins" \(0\) must be greater than or equal to the number of subscriptions \(1\) on the old cluster/
	],
	[qr//],
	'run of pg_upgrade where the new cluster has insufficient max_active_replication_origins'
);

# Reset max_active_replication_origins
$new_sub->append_conf('postgresql.conf',
	"max_active_replication_origins = 10");

# Cleanup
$publisher->safe_psql('postgres', "DROP PUBLICATION regress_pub1");
$old_sub->start;
$old_sub->safe_psql('postgres', "DROP SUBSCRIPTION regress_sub1;");

# ------------------------------------------------------
# Check that pg_upgrade refuses to run if:
# a) there's a subscription with tables in a state other than 'r' (ready) or
#    'i' (init) and/or
# b) the subscription has no replication origin.
# ------------------------------------------------------
$publisher->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_primary_key(id serial PRIMARY KEY);
		INSERT INTO tab_primary_key values(1);
		CREATE PUBLICATION regress_pub2 FOR TABLE tab_primary_key;
]);

# Insert the same value that is already present in publisher to the primary key
# column of subscriber so that the table sync will fail.
$old_sub->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_primary_key(id serial PRIMARY KEY);
		INSERT INTO tab_primary_key values(1);
		CREATE SUBSCRIPTION regress_sub2 CONNECTION '$connstr' PUBLICATION regress_pub2;
]);

# Table will be in 'd' (data is being copied) state as table sync will fail
# because of primary key constraint error.
my $started_query =
  "SELECT count(1) = 1 FROM pg_subscription_rel WHERE srsubstate = 'd'";
$old_sub->poll_query_until('postgres', $started_query)
  or die
  "Timed out while waiting for the table state to become 'd' (datasync)";

# Setup another logical replication and drop the subscription's replication
# origin.
$publisher->safe_psql('postgres', "CREATE PUBLICATION regress_pub3");
$old_sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_sub3 CONNECTION '$connstr' PUBLICATION regress_pub3 WITH (enabled = false)"
);
my $sub_oid = $old_sub->safe_psql('postgres',
	"SELECT oid FROM pg_subscription WHERE subname = 'regress_sub3'");
my $reporigin = 'pg_' . qq($sub_oid);
$old_sub->safe_psql('postgres',
	"SELECT pg_replication_origin_drop('$reporigin')");

$old_sub->stop;

command_checks_all(
	[
		'pg_upgrade',
		'--no-sync',
		'--old-datadir' => $old_sub->data_dir,
		'--new-datadir' => $new_sub->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $new_sub->host,
		'--old-port' => $old_sub->port,
		'--new-port' => $new_sub->port,
		$mode,
		'--check',
	],
	1,
	[
		qr/\QYour installation contains subscriptions without origin or having relations not in i (initialize) or r (ready) state\E/
	],
	[],
	'run of pg_upgrade --check for old instance with relation in \'d\' datasync(invalid) state and missing replication origin'
);

# Verify the reason why the subscriber cannot be upgraded
my $sub_relstate_filename;

# Find a txt file that contains a list of tables that cannot be upgraded. We
# cannot predict the file's path because the output directory contains a
# milliseconds timestamp. File::Find::find must be used.
find(
	sub {
		if ($File::Find::name =~ m/subs_invalid\.txt/)
		{
			$sub_relstate_filename = $File::Find::name;
		}
	},
	$new_sub->data_dir . "/pg_upgrade_output.d");

# Check the file content which should have tab_primary_key table in an invalid
# state.
like(
	slurp_file($sub_relstate_filename),
	qr/The table sync state \"d\" is not allowed for database:\"postgres\" subscription:\"regress_sub2\" schema:\"public\" relation:\"tab_primary_key\"/m,
	'the previous test failed due to subscription table in invalid state');

# Check the file content which should have regress_sub3 subscription.
like(
	slurp_file($sub_relstate_filename),
	qr/The replication origin is missing for database:\"postgres\" subscription:\"regress_sub3\"/m,
	'the previous test failed due to missing replication origin');

# Cleanup
$old_sub->start;
$publisher->safe_psql(
	'postgres', qq[
		DROP PUBLICATION regress_pub2;
		DROP PUBLICATION regress_pub3;
		DROP TABLE tab_primary_key;
]);
$old_sub->safe_psql(
	'postgres', qq[
		DROP SUBSCRIPTION regress_sub2;
		DROP SUBSCRIPTION regress_sub3;
		DROP TABLE tab_primary_key;
]);
rmtree($new_sub->data_dir . "/pg_upgrade_output.d");

# Verify that the upgrade should be successful with tables in 'ready'/'init'
# state along with retaining the replication origin's remote lsn, subscription's
# running status, and failover option.
$publisher->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_upgraded1(id int);
		CREATE PUBLICATION regress_pub4 FOR TABLE tab_upgraded1;
]);

$old_sub->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_upgraded1(id int);
		CREATE SUBSCRIPTION regress_sub4 CONNECTION '$connstr' PUBLICATION regress_pub4 WITH (failover = true);
]);

# Wait till the table tab_upgraded1 reaches 'ready' state
my $synced_query =
  "SELECT count(1) = 1 FROM pg_subscription_rel WHERE srsubstate = 'r'";
$old_sub->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for the table to reach ready state";

$publisher->safe_psql('postgres',
	"INSERT INTO tab_upgraded1 VALUES (generate_series(1,50))");
$publisher->wait_for_catchup('regress_sub4');

# Change configuration to prepare a subscription table in init state
$old_sub->append_conf('postgresql.conf',
	"max_logical_replication_workers = 0");
$old_sub->restart;

# Setup another logical replication
$publisher->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_upgraded2(id int);
		CREATE PUBLICATION regress_pub5 FOR TABLE tab_upgraded2;
]);
$old_sub->safe_psql(
	'postgres', qq[
		CREATE TABLE tab_upgraded2(id int);
		CREATE SUBSCRIPTION regress_sub5 CONNECTION '$connstr' PUBLICATION regress_pub5;
]);

# The table tab_upgraded2 will be in the init state as the subscriber's
# configuration for max_logical_replication_workers is set to 0.
my $result = $old_sub->safe_psql('postgres',
	"SELECT count(1) = 1 FROM pg_subscription_rel WHERE srsubstate = 'i'");
is($result, qq(t), "Check that the table is in init state");

# Get the replication origin's remote_lsn of the old subscriber
my $remote_lsn = $old_sub->safe_psql('postgres',
	"SELECT remote_lsn FROM pg_replication_origin_status os, pg_subscription s WHERE os.external_id = 'pg_' || s.oid AND s.subname = 'regress_sub4'"
);
# Have the subscription in disabled state before upgrade
$old_sub->safe_psql('postgres', "ALTER SUBSCRIPTION regress_sub5 DISABLE");

my $tab_upgraded1_oid = $old_sub->safe_psql('postgres',
	"SELECT oid FROM pg_class WHERE relname = 'tab_upgraded1'");
my $tab_upgraded2_oid = $old_sub->safe_psql('postgres',
	"SELECT oid FROM pg_class WHERE relname = 'tab_upgraded2'");

$old_sub->stop;

# Change configuration so that initial table sync does not get started
# automatically
$new_sub->append_conf('postgresql.conf',
	"max_logical_replication_workers = 0");

# ------------------------------------------------------
# Check that pg_upgrade is successful when all tables are in ready or in
# init state (tab_upgraded1 table is in ready state and tab_upgraded2 table is
# in init state) along with retaining the replication origin's remote lsn,
# subscription's running status, and failover option.
# ------------------------------------------------------
command_ok(
	[
		'pg_upgrade',
		'--no-sync',
		'--old-datadir' => $old_sub->data_dir,
		'--new-datadir' => $new_sub->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $new_sub->host,
		'--old-port' => $old_sub->port,
		'--new-port' => $new_sub->port,
		$mode
	],
	'run of pg_upgrade for old instance when the subscription tables are in init/ready state'
);
ok( !-d $new_sub->data_dir . "/pg_upgrade_output.d",
	"pg_upgrade_output.d/ removed after successful pg_upgrade");

# ------------------------------------------------------
# Check that the data inserted to the publisher when the new subscriber is down
# will be replicated once it is started. Also check that the old subscription
# states and relations origins are all preserved.
# ------------------------------------------------------
$publisher->safe_psql(
	'postgres', qq[
		INSERT INTO tab_upgraded1 VALUES(51);
		INSERT INTO tab_upgraded2 VALUES(1);
]);

$new_sub->start;

# The subscription's running status and failover option should be preserved
# in the upgraded instance. So regress_sub4 should still have subenabled and
# subfailover set to true, while regress_sub5 should have both set to false.
$result = $new_sub->safe_psql('postgres',
	"SELECT subname, subenabled, subfailover FROM pg_subscription ORDER BY subname"
);
is( $result, qq(regress_sub4|t|t
regress_sub5|f|f),
	"check that the subscription's running status and failover are preserved"
);

# Subscription relations should be preserved
$result = $new_sub->safe_psql('postgres',
	"SELECT srrelid, srsubstate FROM pg_subscription_rel ORDER BY srrelid");
is( $result, qq($tab_upgraded1_oid|r
$tab_upgraded2_oid|i),
	"there should be 2 rows in pg_subscription_rel(representing tab_upgraded1 and tab_upgraded2)"
);

# The replication origin's remote_lsn should be preserved
$sub_oid = $new_sub->safe_psql('postgres',
	"SELECT oid FROM pg_subscription WHERE subname = 'regress_sub4'");
$result = $new_sub->safe_psql('postgres',
	"SELECT remote_lsn FROM pg_replication_origin_status WHERE external_id = 'pg_' || $sub_oid"
);
is($result, qq($remote_lsn), "remote_lsn should have been preserved");

# Resume the initial sync and wait until all tables of subscription
# 'regress_sub5' are synchronized
$new_sub->append_conf('postgresql.conf',
	"max_logical_replication_workers = 10");
$new_sub->restart;
$new_sub->safe_psql('postgres', "ALTER SUBSCRIPTION regress_sub5 ENABLE");
$new_sub->wait_for_subscription_sync($publisher, 'regress_sub5');

# wait for regress_sub4 to catchup as well
$publisher->wait_for_catchup('regress_sub4');

# Rows on tab_upgraded1 and tab_upgraded2 should have been replicated
$result =
  $new_sub->safe_psql('postgres', "SELECT count(*) FROM tab_upgraded1");
is($result, qq(51), "check replicated inserts on new subscriber");
$result =
  $new_sub->safe_psql('postgres', "SELECT count(*) FROM tab_upgraded2");
is($result, qq(1),
	"check the data is synced after enabling the subscription for the table that was in init state"
);

done_testing();
