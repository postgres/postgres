
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# logical replication of 2PC test
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

###############################
# Setup
###############################

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	qq(max_prepared_transactions = 10));
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf('postgresql.conf',
	qq(max_prepared_transactions = 0));
$node_subscriber->start;

# Create some pre-existing content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_full (a int PRIMARY KEY)");
$node_publisher->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO tab_full SELECT generate_series(1,10);
	PREPARE TRANSACTION 'some_initial_data';
	COMMIT PREPARED 'some_initial_data';");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_full (a int PRIMARY KEY)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE tab_full");

my $appname = 'tap_sub';
$node_subscriber->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION tap_sub
	CONNECTION '$publisher_connstr application_name=$appname'
	PUBLICATION tap_pub
	WITH (two_phase = on)");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# Also wait for two-phase to be enabled
my $twophase_query =
  "SELECT count(1) = 0 FROM pg_subscription WHERE subtwophasestate NOT IN ('e');";
$node_subscriber->poll_query_until('postgres', $twophase_query)
  or die "Timed out while waiting for subscriber to enable twophase";

###############################
# check that 2PC gets replicated to subscriber
# then COMMIT PREPARED
###############################

# Save the log location, to see the failure of the application
my $log_location = -s $node_subscriber->logfile;

$node_publisher->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO tab_full VALUES (11);
	PREPARE TRANSACTION 'test_prepared_tab_full';");

# Confirm the ERROR is reported because max_prepared_transactions is zero
$node_subscriber->wait_for_log(
	qr/ERROR: ( [A-Z0-9]+:)? prepared transactions are disabled/);

# Set max_prepared_transactions to correct value to resume the replication
$node_subscriber->append_conf('postgresql.conf',
	qq(max_prepared_transactions = 10));
$node_subscriber->restart;

$node_publisher->wait_for_catchup($appname);

# check that transaction is in prepared state on subscriber
my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber');

# check that 2PC gets committed on subscriber
$node_publisher->safe_psql('postgres',
	"COMMIT PREPARED 'test_prepared_tab_full';");

$node_publisher->wait_for_catchup($appname);

# check that transaction is committed on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a = 11;");
is($result, qq(1), 'Row inserted via 2PC has committed on subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is committed on subscriber');

###############################
# check that 2PC gets replicated to subscriber
# then ROLLBACK PREPARED
###############################

$node_publisher->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO tab_full VALUES (12);
	PREPARE TRANSACTION 'test_prepared_tab_full';");

$node_publisher->wait_for_catchup($appname);

# check that transaction is in prepared state on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber');

# check that 2PC gets aborted on subscriber
$node_publisher->safe_psql('postgres',
	"ROLLBACK PREPARED 'test_prepared_tab_full';");

$node_publisher->wait_for_catchup($appname);

# check that transaction is aborted on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a = 12;");
is($result, qq(0), 'Row inserted via 2PC is not present on subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is aborted on subscriber');

###############################
# Check that ROLLBACK PREPARED is decoded properly on crash restart
# (publisher and subscriber crash)
###############################

$node_publisher->safe_psql(
	'postgres', "
    BEGIN;
    INSERT INTO tab_full VALUES (12);
    INSERT INTO tab_full VALUES (13);
    PREPARE TRANSACTION 'test_prepared_tab';");

$node_subscriber->stop('immediate');
$node_publisher->stop('immediate');

$node_publisher->start;
$node_subscriber->start;

# rollback post the restart
$node_publisher->safe_psql('postgres',
	"ROLLBACK PREPARED 'test_prepared_tab';");
$node_publisher->wait_for_catchup($appname);

# check inserts are rolled back
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a IN (12,13);");
is($result, qq(0), 'Rows rolled back are not on the subscriber');

###############################
# Check that COMMIT PREPARED is decoded properly on crash restart
# (publisher and subscriber crash)
###############################

$node_publisher->safe_psql(
	'postgres', "
    BEGIN;
    INSERT INTO tab_full VALUES (12);
    INSERT INTO tab_full VALUES (13);
    PREPARE TRANSACTION 'test_prepared_tab';");

$node_subscriber->stop('immediate');
$node_publisher->stop('immediate');

$node_publisher->start;
$node_subscriber->start;

# commit post the restart
$node_publisher->safe_psql('postgres',
	"COMMIT PREPARED 'test_prepared_tab';");
$node_publisher->wait_for_catchup($appname);

# check inserts are visible
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a IN (12,13);");
is($result, qq(2), 'Rows inserted via 2PC are visible on the subscriber');

###############################
# Check that COMMIT PREPARED is decoded properly on crash restart
# (subscriber only crash)
###############################

$node_publisher->safe_psql(
	'postgres', "
    BEGIN;
    INSERT INTO tab_full VALUES (14);
    INSERT INTO tab_full VALUES (15);
    PREPARE TRANSACTION 'test_prepared_tab';");

$node_subscriber->stop('immediate');
$node_subscriber->start;

# commit post the restart
$node_publisher->safe_psql('postgres',
	"COMMIT PREPARED 'test_prepared_tab';");
$node_publisher->wait_for_catchup($appname);

# check inserts are visible
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a IN (14,15);");
is($result, qq(2), 'Rows inserted via 2PC are visible on the subscriber');

###############################
# Check that COMMIT PREPARED is decoded properly on crash restart
# (publisher only crash)
###############################

$node_publisher->safe_psql(
	'postgres', "
    BEGIN;
    INSERT INTO tab_full VALUES (16);
    INSERT INTO tab_full VALUES (17);
    PREPARE TRANSACTION 'test_prepared_tab';");

$node_publisher->stop('immediate');
$node_publisher->start;

# commit post the restart
$node_publisher->safe_psql('postgres',
	"COMMIT PREPARED 'test_prepared_tab';");
$node_publisher->wait_for_catchup($appname);

# check inserts are visible
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a IN (16,17);");
is($result, qq(2), 'Rows inserted via 2PC are visible on the subscriber');

###############################
# Test nested transaction with 2PC
###############################

# check that 2PC gets replicated to subscriber
$node_publisher->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO tab_full VALUES (21);
	SAVEPOINT sp_inner;
	INSERT INTO tab_full VALUES (22);
	ROLLBACK TO SAVEPOINT sp_inner;
	PREPARE TRANSACTION 'outer';
	");
$node_publisher->wait_for_catchup($appname);

# check that transaction is in prepared state on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber');

# COMMIT
$node_publisher->safe_psql('postgres', "COMMIT PREPARED 'outer';");

$node_publisher->wait_for_catchup($appname);

# check the transaction state on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is ended on subscriber');

# check inserts are visible. 22 should be rolled back. 21 should be committed.
$result = $node_subscriber->safe_psql('postgres',
	"SELECT a FROM tab_full where a IN (21,22);");
is($result, qq(21), 'Rows committed are on the subscriber');

###############################
# Test using empty GID
###############################

# check that 2PC gets replicated to subscriber
$node_publisher->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO tab_full VALUES (51);
	PREPARE TRANSACTION '';");
$node_publisher->wait_for_catchup($appname);

# check that transaction is in prepared state on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber');

# ROLLBACK
$node_publisher->safe_psql('postgres', "ROLLBACK PREPARED '';");

# check that 2PC gets aborted on subscriber
$node_publisher->wait_for_catchup($appname);

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is aborted on subscriber');

###############################
# copy_data=false and two_phase
###############################

#create some test tables for copy tests
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_copy (a int PRIMARY KEY)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_copy SELECT generate_series(1,5);");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_copy (a int PRIMARY KEY)");
$node_subscriber->safe_psql('postgres', "INSERT INTO tab_copy VALUES (88);");
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_copy;");
is($result, qq(1), 'initial data in subscriber table');

# Setup logical replication
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_copy FOR TABLE tab_copy;");

my $appname_copy = 'appname_copy';
$node_subscriber->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION tap_sub_copy
	CONNECTION '$publisher_connstr application_name=$appname_copy'
	PUBLICATION tap_pub_copy
	WITH (two_phase=on, copy_data=false);");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname_copy);

# Also wait for two-phase to be enabled
$node_subscriber->poll_query_until('postgres', $twophase_query)
  or die "Timed out while waiting for subscriber to enable twophase";

# Check that the initial table data was NOT replicated (because we said copy_data=false)
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_copy;");
is($result, qq(1), 'initial data in subscriber table');

# Now do a prepare on publisher and check that it IS replicated
$node_publisher->safe_psql(
	'postgres', "
    BEGIN;
    INSERT INTO tab_copy VALUES (99);
    PREPARE TRANSACTION 'mygid';");

# Wait for both subscribers to catchup
$node_publisher->wait_for_catchup($appname_copy);
$node_publisher->wait_for_catchup($appname);

# Check that the transaction has been prepared on the subscriber, there will be 2
# prepared transactions for the 2 subscriptions.
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(2), 'transaction is prepared on subscriber');

# Now commit the insert and verify that it IS replicated
$node_publisher->safe_psql('postgres', "COMMIT PREPARED 'mygid';");

$result =
  $node_publisher->safe_psql('postgres', "SELECT count(*) FROM tab_copy;");
is($result, qq(6), 'publisher inserted data');

# Wait for both subscribers to catchup
$node_publisher->wait_for_catchup($appname_copy);
$node_publisher->wait_for_catchup($appname);

# Make sure there are no prepared transactions on the subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'should be no prepared transactions on subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_copy;");
is($result, qq(2), 'replicated data in subscriber table');

# Clean up
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");

###############################
# Alter the subscription to set two_phase to false.
# Verify that the altered subscription reflects the new two_phase option.
###############################

# Confirm that the two-phase slot option is enabled before altering
$result = $node_publisher->safe_psql('postgres',
	"SELECT two_phase FROM pg_replication_slots WHERE slot_name = 'tap_sub_copy';"
);
is($result, qq(t), 'two-phase is enabled');

# Alter subscription two_phase to false
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_copy DISABLE;");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);
$node_subscriber->safe_psql(
	'postgres', "
    ALTER SUBSCRIPTION tap_sub_copy SET (two_phase = false);
    ALTER SUBSCRIPTION tap_sub_copy ENABLE;");

# Wait for subscription startup
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname_copy);

# Make sure that the two-phase is disabled on the subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT subtwophasestate FROM pg_subscription WHERE subname = 'tap_sub_copy';"
);
is($result, qq(d), 'two-phase subscription option should be disabled');

# Make sure that the two-phase slot option is also disabled
$result = $node_publisher->safe_psql('postgres',
	"SELECT two_phase FROM pg_replication_slots WHERE slot_name = 'tap_sub_copy';"
);
is($result, qq(f), 'two-phase slot option should be disabled');

###############################
# Now do a prepare on the publisher and verify that it is not replicated.
###############################
$node_publisher->safe_psql(
	'postgres', qq{
    BEGIN;
    INSERT INTO tab_copy VALUES (100);
    PREPARE TRANSACTION 'newgid';
	});

# Wait for the subscriber to catchup
$node_publisher->wait_for_catchup($appname_copy);

# Make sure there are no prepared transactions on the subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'should be no prepared transactions on subscriber');

###############################
# Set two_phase to "true" and failover to "true" before the COMMIT PREPARED.
#
# This tests the scenario where both two_phase and failover are altered
# simultaneously.
###############################
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_copy DISABLE;");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);
$node_subscriber->safe_psql(
	'postgres', "
    ALTER SUBSCRIPTION tap_sub_copy SET (two_phase = true, failover = true);
    ALTER SUBSCRIPTION tap_sub_copy ENABLE;");

###############################
# Now commit the insert and verify that it is replicated.
###############################
$node_publisher->safe_psql('postgres', "COMMIT PREPARED 'newgid';");

# Wait for the subscriber to catchup
$node_publisher->wait_for_catchup($appname_copy);

# Make sure that the committed transaction is replicated.
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_copy;");
is($result, qq(3), 'replicated data in subscriber table');

# Make sure that the two-phase is enabled on the subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT subtwophasestate FROM pg_subscription WHERE subname = 'tap_sub_copy';"
);
is($result, qq(e), 'two-phase should be enabled');

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_copy;");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_copy;");

###############################
# check all the cleanup
###############################

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_subscription");
is($result, qq(0), 'check subscription was dropped on subscriber');

$result = $node_publisher->safe_psql('postgres',
	"SELECT count(*) FROM pg_replication_slots");
is($result, qq(0), 'check replication slot was dropped on publisher');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_subscription_rel");
is($result, qq(0),
	'check subscription relation status was dropped on subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_replication_origin");
is($result, qq(0), 'check replication origin was dropped on subscriber');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
