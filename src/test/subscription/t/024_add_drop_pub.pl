
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# This test checks behaviour of ALTER SUBSCRIPTION ... ADD/DROP PUBLICATION
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

# shutdown
$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
