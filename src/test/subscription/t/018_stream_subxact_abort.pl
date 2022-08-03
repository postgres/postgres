
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test streaming of large transaction containing multiple subtransactions and rollbacks
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	'logical_decoding_work_mem = 64kB');
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

# large (streamed) transaction with DDL, DML and ROLLBACKs
$node_publisher->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(3,500) s(i);
SAVEPOINT s1;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(501,1000) s(i);
SAVEPOINT s2;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(1001,1500) s(i);
SAVEPOINT s3;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(1501,2000) s(i);
ROLLBACK TO s2;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(2001,2500) s(i);
ROLLBACK TO s1;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(2501,3000) s(i);
SAVEPOINT s4;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(3001,3500) s(i);
SAVEPOINT s5;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(3501,4000) s(i);
COMMIT;
});

$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c) FROM test_tab");
is($result, qq(2000|0),
	'check rollback to savepoint was reflected on subscriber and extra columns contain local defaults'
);

# large (streamed) transaction with subscriber receiving out of order
# subtransaction ROLLBACKs
$node_publisher->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(4001,4500) s(i);
SAVEPOINT s1;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(5001,5500) s(i);
SAVEPOINT s2;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(6001,6500) s(i);
SAVEPOINT s3;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(7001,7500) s(i);
RELEASE s2;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(8001,8500) s(i);
ROLLBACK TO s1;
COMMIT;
});

$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c) FROM test_tab");
is($result, qq(2500|0),
	'check rollback to savepoint was reflected on subscriber');

# large (streamed) transaction with subscriber receiving rollback
$node_publisher->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(8501,9000) s(i);
SAVEPOINT s1;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(9001,9500) s(i);
SAVEPOINT s2;
INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(9501,10000) s(i);
ROLLBACK;
});

$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c) FROM test_tab");
is($result, qq(2500|0), 'check rollback was reflected on subscriber');

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
