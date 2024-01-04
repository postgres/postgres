
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test logical replication behavior with heap rewrites
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

my $ddl = "CREATE TABLE test1 (a int, b text);";
$node_publisher->safe_psql('postgres', $ddl);
$node_subscriber->safe_psql('postgres', $ddl);

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION mypub FOR ALL TABLES;");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION mysub CONNECTION '$publisher_connstr' PUBLICATION mypub;"
);

# Wait for initial sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'mysub');

$node_publisher->safe_psql('postgres',
	q{INSERT INTO test1 (a, b) VALUES (1, 'one'), (2, 'two');});

$node_publisher->wait_for_catchup('mysub');

is( $node_subscriber->safe_psql('postgres', q{SELECT a, b FROM test1}),
	qq(1|one
2|two),
	'initial data replicated to subscriber');

# DDL that causes a heap rewrite
my $ddl2 = "ALTER TABLE test1 ADD c int NOT NULL DEFAULT 0;";
$node_subscriber->safe_psql('postgres', $ddl2);
$node_publisher->safe_psql('postgres', $ddl2);

$node_publisher->wait_for_catchup('mysub');

$node_publisher->safe_psql('postgres',
	q{INSERT INTO test1 (a, b, c) VALUES (3, 'three', 33);});

$node_publisher->wait_for_catchup('mysub');

is( $node_subscriber->safe_psql('postgres', q{SELECT a, b, c FROM test1}),
	qq(1|one|0
2|two|0
3|three|33),
	'data replicated to subscriber');

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
