
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test generated columns
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# setup

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, b int GENERATED ALWAYS AS (a * 2) STORED)"
);

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, b int GENERATED ALWAYS AS (a * 22) STORED)"
);

# data for initial sync

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1 (a) VALUES (1), (2), (3)");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);

# Wait for initial sync of all subscriptions
$node_subscriber->wait_for_subscription_sync;

my $result = $node_subscriber->safe_psql('postgres', "SELECT a, b FROM tab1");
is( $result, qq(1|22
2|44
3|66), 'generated columns initial sync');

# data to replicate

$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (4), (5)");

$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 6 WHERE a = 5");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres', "SELECT a, b FROM tab1");
is( $result, qq(1|22
2|44
3|66
4|88
6|132), 'generated columns replicated');

done_testing();
