
# Copyright (c) 2021-2023, PostgreSQL Global Development Group

# Test streaming of transaction containing multiple subtransactions and rollbacks
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	'logical_decoding_mode = immediate');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Create some preexisting content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b varchar)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab VALUES (1, 'foo'), (2, 'bar')");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b text, c INT, d INT, e INT)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE test_tab");

my $appname = 'tap_sub';
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub WITH (streaming = on)"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

my $result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c) FROM test_tab");
is($result, qq(2|0), 'check initial data was copied to subscriber');

# streamed transaction with DDL, DML and ROLLBACKs
$node_publisher->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO test_tab VALUES (3, md5(3::text));
SAVEPOINT s1;
INSERT INTO test_tab VALUES (4, md5(4::text));
SAVEPOINT s2;
INSERT INTO test_tab VALUES (5, md5(5::text));
SAVEPOINT s3;
INSERT INTO test_tab VALUES (6, md5(6::text));
ROLLBACK TO s2;
INSERT INTO test_tab VALUES (7, md5(7::text));
ROLLBACK TO s1;
INSERT INTO test_tab VALUES (8, md5(8::text));
SAVEPOINT s4;
INSERT INTO test_tab VALUES (9, md5(9::text));
SAVEPOINT s5;
INSERT INTO test_tab VALUES (10, md5(10::text));
COMMIT;
});

$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c) FROM test_tab");
is($result, qq(6|0),
	'check rollback to savepoint was reflected on subscriber and extra columns contain local defaults'
);

# streamed transaction with subscriber receiving out of order subtransaction
# ROLLBACKs
$node_publisher->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO test_tab VALUES (11, md5(11::text));
SAVEPOINT s1;
INSERT INTO test_tab VALUES (12, md5(12::text));
SAVEPOINT s2;
INSERT INTO test_tab VALUES (13, md5(13::text));
SAVEPOINT s3;
INSERT INTO test_tab VALUES (14, md5(14::text));
RELEASE s2;
INSERT INTO test_tab VALUES (15, md5(15::text));
ROLLBACK TO s1;
COMMIT;
});

$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c) FROM test_tab");
is($result, qq(7|0),
	'check rollback to savepoint was reflected on subscriber');

# streamed transaction with subscriber receiving rollback
$node_publisher->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO test_tab VALUES (16, md5(16::text));
SAVEPOINT s1;
INSERT INTO test_tab VALUES (17, md5(17::text));
SAVEPOINT s2;
INSERT INTO test_tab VALUES (18, md5(18::text));
ROLLBACK;
});

$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c) FROM test_tab");
is($result, qq(7|0), 'check rollback was reflected on subscriber');

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
