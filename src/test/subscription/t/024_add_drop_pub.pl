
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# This test checks behaviour of ALTER SUBSCRIPTION ... ADD/DROP PUBLICATION and
# ensures that creating a publication associated with a subscription at a later
# point of time does not break logical replication.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Create table on publisher
$node_publisher->safe_psql('postgres', "CREATE TABLE tab_1 (a int)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_1 SELECT generate_series(1,10)");

# Create table on subscriber
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_1 (a int)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_1 FOR TABLE tab_1");
$node_publisher->safe_psql('postgres', "CREATE PUBLICATION tap_pub_2");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub_1, tap_pub_2"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Check the initial data of tab_1 is copied to subscriber
my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_1");
is($result, qq(10|1|10), 'check initial data is copied to subscriber');

# Create a new table on publisher
$node_publisher->safe_psql('postgres', "CREATE TABLE tab_2 (a int)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_2 SELECT generate_series(1,10)");

# Create a new table on subscriber
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_2 (a int)");

# Add the table to publication
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_2 ADD TABLE tab_2");

# Dropping tap_pub_1 will refresh the entire publication list
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub DROP PUBLICATION tap_pub_1");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Check the initial data of tab_2 was copied to subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_2");
is($result, qq(10|1|10), 'check initial data is copied to subscriber');

# Re-adding tap_pub_1 will refresh the entire publication list
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub ADD PUBLICATION tap_pub_1");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Check the initial data of tab_1 was copied to subscriber again
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab_1");
is($result, qq(20|1|10), 'check initial data is copied to subscriber');

# Ensure that setting a missing publication to the subscription does not
# disrupt existing logical replication. Instead, it should log a warning
# while allowing replication to continue. Additionally, verify that replication
# resumes after the missing publication is created for the publication table.

# Create table on publisher and subscriber
$node_publisher->safe_psql('postgres', "CREATE TABLE tab_3 (a int)");
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_3 (a int)");

my $oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
);

# Set the subscription with a missing publication
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub SET PUBLICATION tap_pub_3");

# Wait for the walsender to restart after altering the subscription
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = 'tap_sub' AND state = 'streaming';"
  )
  or die
  "Timed out while waiting for apply worker to restart after altering the subscription";

my $offset = -s $node_publisher->logfile;

$node_publisher->safe_psql('postgres',"INSERT INTO tab_3 values(1)");

# Verify that a warning is logged.
$node_publisher->wait_for_log(
	qr/WARNING: ( [A-Z0-9]+:)? skipped loading publication "tap_pub_3"/, $offset);

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_3 FOR TABLE tab_3");

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH  PUBLICATION");

$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', "INSERT INTO tab_3 values(2)");

$node_publisher->wait_for_catchup('tap_sub');

# Verify that the insert operation gets replicated to subscriber after
# publication is created.
$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM tab_3");
is($result, qq(1
2), 'check that the incremental data is replicated after the publication is created');

# shutdown
$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
