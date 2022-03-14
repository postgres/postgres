
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test of logical replication subscription self-disabling feature.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# create publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Create identical table on both nodes.
$node_publisher->safe_psql('postgres', "CREATE TABLE tbl (i INT)");
$node_subscriber->safe_psql('postgres', "CREATE TABLE tbl (i INT)");

# Insert duplicate values on the publisher.
$node_publisher->safe_psql('postgres',
	"INSERT INTO tbl (i) VALUES (1), (1), (1)");

# Create an additional unique index on the subscriber.
$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX tbl_unique ON tbl (i)");

# Create a pub/sub to set up logical replication. This tests that the
# uniqueness violation will cause the subscription to fail during initial
# synchronization and make it disabled.
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub FOR TABLE tbl");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$publisher_connstr' PUBLICATION pub WITH (disable_on_error = true)"
);

# Initial synchronization failure causes the subscription to be disabled.
$node_subscriber->poll_query_until('postgres',
	"SELECT subenabled = false FROM pg_catalog.pg_subscription WHERE subname = 'sub'"
) or die "Timed out while waiting for subscriber to be disabled";

# Drop the unique index on the subscriber which caused the subscription to be
# disabled.
$node_subscriber->safe_psql('postgres', "DROP INDEX tbl_unique");

# Re-enable the subscription "sub".
$node_subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION sub ENABLE");

# Wait for the data to replicate.
$node_publisher->wait_for_catchup('sub');
$node_subscriber->poll_query_until('postgres',
	"SELECT COUNT(1) = 0 FROM pg_subscription_rel sr WHERE sr.srsubstate NOT IN ('s', 'r') AND sr.srrelid = 'tbl'::regclass"
);

# Confirm that we have finished the table sync.
my $result =
  $node_subscriber->safe_psql('postgres', "SELECT MAX(i), COUNT(*) FROM tbl");
is($result, qq(1|3), "subscription sub replicated data");

# Delete the data from the subscriber and recreate the unique index.
$node_subscriber->safe_psql('postgres', "DELETE FROM tbl");
$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX tbl_unique ON tbl (i)");

# Add more non-unique data to the publisher.
$node_publisher->safe_psql('postgres',
	"INSERT INTO tbl (i) VALUES (3), (3), (3)");

# Apply failure causes the subscription to be disabled.
$node_subscriber->poll_query_until('postgres',
	"SELECT subenabled = false FROM pg_catalog.pg_subscription WHERE subname = 'sub'"
) or die "Timed out while waiting for subscription sub to be disabled";

# Drop the unique index on the subscriber and re-enabled the subscription. Then
# confirm that the previously failing insert was applied OK.
$node_subscriber->safe_psql('postgres', "DROP INDEX tbl_unique");
$node_subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION sub ENABLE");

$node_publisher->wait_for_catchup('sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT COUNT(*) FROM tbl WHERE i = 3");
is($result, qq(3), 'check the result of apply');

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
