
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test logical replication of 2PC with streaming.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

###############################
# Setup
###############################

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf(
	'postgresql.conf', qq(
max_prepared_transactions = 10
logical_decoding_work_mem = 64kB
));
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->append_conf(
	'postgresql.conf', qq(
max_prepared_transactions = 10
));
$node_subscriber->start;

# Create some pre-existing content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b varchar)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab VALUES (1, 'foo'), (2, 'bar')");

# Setup structure on subscriber (columns a and b are compatible with same table name on publisher)
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b text, c timestamptz DEFAULT now(), d bigint DEFAULT 999)"
);

# Setup logical replication (streaming = on)
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE test_tab");

my $appname = 'tap_sub';
$node_subscriber->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION tap_sub
	CONNECTION '$publisher_connstr application_name=$appname'
	PUBLICATION tap_pub
	WITH (streaming = on, two_phase = on)");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# Also wait for two-phase to be enabled
my $twophase_query =
  "SELECT count(1) = 0 FROM pg_subscription WHERE subtwophasestate NOT IN ('e');";
$node_subscriber->poll_query_until('postgres', $twophase_query)
  or die "Timed out while waiting for subscriber to enable twophase";

###############################
# Check initial data was copied to subscriber
###############################
my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(2|2|2), 'check initial data was copied to subscriber');

###############################
# Test 2PC PREPARE / COMMIT PREPARED.
# 1. Data is streamed as a 2PC transaction.
# 2. Then do commit prepared.
#
# Expect all data is replicated on subscriber side after the commit.
###############################

# check that 2PC gets replicated to subscriber
# Insert, update and delete enough rows to exceed the 64kB limit.
$node_publisher->safe_psql(
	'postgres', q{
	BEGIN;
	INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(3, 5000) s(i);
	UPDATE test_tab SET b = md5(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	PREPARE TRANSACTION 'test_prepared_tab';});

$node_publisher->wait_for_catchup($appname);

# check that transaction is in prepared state on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber');

# 2PC transaction gets committed
$node_publisher->safe_psql('postgres',
	"COMMIT PREPARED 'test_prepared_tab';");

$node_publisher->wait_for_catchup($appname);

# check that transaction is committed on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(3334|3334|3334),
	'Rows inserted by 2PC have committed on subscriber, and extra columns contain local defaults'
);
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is committed on subscriber');

###############################
# Test 2PC PREPARE / ROLLBACK PREPARED.
# 1. Table is deleted back to 2 rows which are replicated on subscriber.
# 2. Data is streamed using 2PC.
# 3. Do rollback prepared.
#
# Expect data rolls back leaving only the original 2 rows.
###############################

# First, delete the data except for 2 rows (will be replicated)
$node_publisher->safe_psql('postgres', "DELETE FROM test_tab WHERE a > 2;");

# Then insert, update and delete enough rows to exceed the 64kB limit.
$node_publisher->safe_psql(
	'postgres', q{
	BEGIN;
	INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(3, 5000) s(i);
	UPDATE test_tab SET b = md5(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	PREPARE TRANSACTION 'test_prepared_tab';});

$node_publisher->wait_for_catchup($appname);

# check that transaction is in prepared state on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber');

# 2PC transaction gets aborted
$node_publisher->safe_psql('postgres',
	"ROLLBACK PREPARED 'test_prepared_tab';");

$node_publisher->wait_for_catchup($appname);

# check that transaction is aborted on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(2|2|2),
	'Rows inserted by 2PC are rolled back, leaving only the original 2 rows');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is aborted on subscriber');

###############################
# Check that 2PC COMMIT PREPARED is decoded properly on crash restart.
# 1. insert, update and delete enough rows to exceed the 64kB limit.
# 2. Then server crashes before the 2PC transaction is committed.
# 3. After servers are restarted the pending transaction is committed.
#
# Expect all data is replicated on subscriber side after the commit.
# Note: both publisher and subscriber do crash/restart.
###############################

$node_publisher->safe_psql(
	'postgres', q{
	BEGIN;
	INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(3, 5000) s(i);
	UPDATE test_tab SET b = md5(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	PREPARE TRANSACTION 'test_prepared_tab';});

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
	"SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(3334|3334|3334),
	'Rows inserted by 2PC have committed on subscriber, and extra columns contain local defaults'
);

###############################
# Do INSERT after the PREPARE but before ROLLBACK PREPARED.
# 1. Table is deleted back to 2 rows which are replicated on subscriber.
# 2. Data is streamed using 2PC.
# 3. A single row INSERT is done which is after the PREPARE.
# 4. Then do a ROLLBACK PREPARED.
#
# Expect the 2PC data rolls back leaving only 3 rows on the subscriber
# (the original 2 + inserted 1).
###############################

# First, delete the data except for 2 rows (will be replicated)
$node_publisher->safe_psql('postgres', "DELETE FROM test_tab WHERE a > 2;");

# Then insert, update and delete enough rows to exceed the 64kB limit.
$node_publisher->safe_psql(
	'postgres', q{
	BEGIN;
	INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(3, 5000) s(i);
	UPDATE test_tab SET b = md5(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	PREPARE TRANSACTION 'test_prepared_tab';});

$node_publisher->wait_for_catchup($appname);

# check that transaction is in prepared state on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber');

# Insert a different record (now we are outside of the 2PC transaction)
# Note: the 2PC transaction still holds row locks so make sure this insert is for a separate primary key
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab VALUES (99999, 'foobar')");

# 2PC transaction gets aborted
$node_publisher->safe_psql('postgres',
	"ROLLBACK PREPARED 'test_prepared_tab';");

$node_publisher->wait_for_catchup($appname);

# check that transaction is aborted on subscriber,
# but the extra INSERT outside of the 2PC still was replicated
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(3|3|3), 'check the outside insert was copied to subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is aborted on subscriber');

###############################
# Do INSERT after the PREPARE but before COMMIT PREPARED.
# 1. Table is deleted back to 2 rows which are replicated on subscriber.
# 2. Data is streamed using 2PC.
# 3. A single row INSERT is done which is after the PREPARE.
# 4. Then do a COMMIT PREPARED.
#
# Expect 2PC data + the extra row are on the subscriber
# (the 3334 + inserted 1 = 3335).
###############################

# First, delete the data except for 2 rows (will be replicated)
$node_publisher->safe_psql('postgres', "DELETE FROM test_tab WHERE a > 2;");

# Then insert, update and delete enough rows to exceed the 64kB limit.
$node_publisher->safe_psql(
	'postgres', q{
	BEGIN;
	INSERT INTO test_tab SELECT i, md5(i::text) FROM generate_series(3, 5000) s(i);
	UPDATE test_tab SET b = md5(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	PREPARE TRANSACTION 'test_prepared_tab';});

$node_publisher->wait_for_catchup($appname);

# check that transaction is in prepared state on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber');

# Insert a different record (now we are outside of the 2PC transaction)
# Note: the 2PC transaction still holds row locks so make sure this insert is for a separare primary key
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab VALUES (99999, 'foobar')");

# 2PC transaction gets committed
$node_publisher->safe_psql('postgres',
	"COMMIT PREPARED 'test_prepared_tab';");

$node_publisher->wait_for_catchup($appname);

# check that transaction is committed on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(3335|3335|3335),
	'Rows inserted by 2PC (as well as outside insert) have committed on subscriber, and extra columns contain local defaults'
);

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is committed on subscriber');

###############################
# check all the cleanup
###############################

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");

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
