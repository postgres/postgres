
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Tests for subscription stats.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create publisher node.
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node.
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Initial table setup on both publisher and subscriber. On subscriber we
# create the same tables but with primary keys. Also, insert some data that
# will conflict with the data replicated from publisher later.
$node_publisher->safe_psql(
	'postgres',
	qq[
BEGIN;
CREATE TABLE test_tab1 (a int);
INSERT INTO test_tab1 VALUES (1);
COMMIT;
]);
$node_subscriber->safe_psql(
	'postgres',
	qq[
BEGIN;
CREATE TABLE test_tab1 (a int primary key);
INSERT INTO test_tab1 VALUES (1);
COMMIT;
]);

# Setup publication.
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE test_tab1;");

# There shouldn't be any subscription errors before starting logical replication.
my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(1) FROM pg_stat_subscription_stats");
is($result, qq(0), 'check no subscription error');

# Create subscription. The tablesync for test_tab1 on tap_sub will enter into
# infinite error loop due to violating the unique constraint.
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub;"
);

$node_publisher->wait_for_catchup('tap_sub');

# Wait for the tablesync error to be reported.
$node_subscriber->poll_query_until(
	'postgres',
	qq[
SELECT sync_error_count > 0
FROM pg_stat_subscription_stats
WHERE subname = 'tap_sub'
]) or die "Timed out while waiting for tablesync error";

# Truncate test_tab1 so that tablesync worker can continue.
$node_subscriber->safe_psql('postgres', "TRUNCATE test_tab1;");

# Wait for initial tablesync for test_tab1 to finish.
$node_subscriber->poll_query_until(
	'postgres',
	qq[
SELECT count(1) = 1 FROM pg_subscription_rel
WHERE srrelid = 'test_tab1'::regclass AND srsubstate in ('r', 's')
]) or die "Timed out while waiting for subscriber to synchronize data";

# Check test_tab1 on the subscriber has one row.
$result = $node_subscriber->safe_psql('postgres', "SELECT a FROM test_tab1");
is($result, qq(1), 'check the table has now row');

# Insert data to test_tab1 on the publisher, raising an error on the subscriber
# due to violation of the unique constraint on test_tab1.
$node_publisher->safe_psql('postgres', "INSERT INTO test_tab1 VALUES (1)");

# Wait for the apply error to be reported.
$node_subscriber->poll_query_until(
	'postgres',
	qq[
SELECT apply_error_count > 0
FROM pg_stat_subscription_stats
WHERE subname = 'tap_sub'
]) or die "Timed out while waiting for apply error";

# Truncate test_tab1 so that apply worker can continue.
$node_subscriber->safe_psql('postgres', "TRUNCATE test_tab1;");

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
