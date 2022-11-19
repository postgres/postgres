
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Tests for disable_on_error and SKIP transaction features.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $offset = 0;

# Test skipping the transaction. This function must be called after the caller
# has inserted data that conflicts with the subscriber.  The finish LSN of the
# error transaction that is used to specify to ALTER SUBSCRIPTION ... SKIP is
# fetched from the server logs. After executing ALTER SUBSCRITPION ... SKIP, we
# check if logical replication can continue working by inserting $nonconflict_data
# on the publisher.
sub test_skip_lsn
{
	my ($node_publisher, $node_subscriber, $nonconflict_data, $expected, $msg)
	  = @_;

	# Wait until a conflict occurs on the subscriber.
	$node_subscriber->poll_query_until('postgres',
		"SELECT subenabled = FALSE FROM pg_subscription WHERE subname = 'sub'"
	);

	# Get the finish LSN of the error transaction.
	my $contents = slurp_file($node_subscriber->logfile, $offset);
	$contents =~
	  qr/processing remote data for replication origin \"pg_\d+\" during message type "INSERT" for replication target relation "public.tbl" in transaction \d+, finished at ([[:xdigit:]]+\/[[:xdigit:]]+)/
	  or die "could not get error-LSN";
	my $lsn = $1;

	# Set skip lsn.
	$node_subscriber->safe_psql('postgres',
		"ALTER SUBSCRIPTION sub SKIP (lsn = '$lsn')");

	# Re-enable the subscription.
	$node_subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION sub ENABLE");

	# Wait for the failed transaction to be skipped
	$node_subscriber->poll_query_until('postgres',
		"SELECT subskiplsn = '0/0' FROM pg_subscription WHERE subname = 'sub'"
	);

	# Check the log to ensure that the transaction is skipped, and advance the
	# offset of the log file for the next test.
	$offset = $node_subscriber->wait_for_log(
		qr/LOG: ( [A-Z0-9]+:)? logical replication completed skipping transaction at LSN $lsn/,
		$offset);

	# Insert non-conflict data
	$node_publisher->safe_psql('postgres',
		"INSERT INTO tbl VALUES $nonconflict_data");

	$node_publisher->wait_for_catchup('sub');

	# Check replicated data
	my $res =
	  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tbl");
	is($res, $expected, $msg);
}

# Create publisher node. Set a low value of logical_decoding_work_mem to test
# streaming cases.
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf(
	'postgresql.conf',
	qq[
logical_decoding_work_mem = 64kB
max_prepared_transactions = 10
]);
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf(
	'postgresql.conf',
	qq[
max_prepared_transactions = 10
]);
$node_subscriber->start;

# Initial table setup on both publisher and subscriber. On the subscriber, we
# create the same tables but with a primary key. Also, insert some data that
# will conflict with the data replicated from publisher later.
$node_publisher->safe_psql(
	'postgres',
	qq[
CREATE TABLE tbl (i INT, t TEXT);
INSERT INTO tbl VALUES (1, NULL);
]);
$node_subscriber->safe_psql(
	'postgres',
	qq[
CREATE TABLE tbl (i INT PRIMARY KEY, t TEXT);
INSERT INTO tbl VALUES (1, NULL);
]);

# Create a pub/sub to set up logical replication. This tests that the
# uniqueness violation will cause the subscription to fail during initial
# synchronization and make it disabled.
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub FOR TABLE tbl");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$publisher_connstr' PUBLICATION pub WITH (disable_on_error = true, streaming = on, two_phase = on)"
);

# Initial synchronization failure causes the subscription to be disabled.
$node_subscriber->poll_query_until('postgres',
	"SELECT subenabled = false FROM pg_catalog.pg_subscription WHERE subname = 'sub'"
) or die "Timed out while waiting for subscriber to be disabled";

# Truncate the table on the subscriber which caused the subscription to be
# disabled.
$node_subscriber->safe_psql('postgres', "TRUNCATE tbl");

# Re-enable the subscription "sub".
$node_subscriber->safe_psql('postgres', "ALTER SUBSCRIPTION sub ENABLE");

# Wait for the data to replicate.
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub');

# Confirm that we have finished the table sync.
my $result =
  $node_subscriber->safe_psql('postgres', "SELECT COUNT(*) FROM tbl");
is($result, qq(1), "subscription sub replicated data");

# Insert data to tbl, raising an error on the subscriber due to violation
# of the unique constraint on tbl. Then skip the transaction.
$node_publisher->safe_psql(
	'postgres',
	qq[
BEGIN;
INSERT INTO tbl VALUES (1, NULL);
COMMIT;
]);
test_skip_lsn($node_publisher, $node_subscriber,
	"(2, NULL)", "2", "test skipping transaction");

# Test for PREPARE and COMMIT PREPARED. Insert the same data to tbl and
# PREPARE the transaction, raising an error. Then skip the transaction.
$node_publisher->safe_psql(
	'postgres',
	qq[
BEGIN;
INSERT INTO tbl VALUES (1, NULL);
PREPARE TRANSACTION 'gtx';
COMMIT PREPARED 'gtx';
]);
test_skip_lsn($node_publisher, $node_subscriber,
	"(3, NULL)", "3", "test skipping prepare and commit prepared ");

# Test for STREAM COMMIT. Insert enough rows to tbl to exceed the 64kB
# limit, also raising an error on the subscriber during applying spooled
# changes for the same reason. Then skip the transaction.
$node_publisher->safe_psql(
	'postgres',
	qq[
BEGIN;
INSERT INTO tbl SELECT i, md5(i::text) FROM generate_series(1, 10000) s(i);
COMMIT;
]);
test_skip_lsn($node_publisher, $node_subscriber, "(4, md5(4::text))",
	"4", "test skipping stream-commit");

$result = $node_subscriber->safe_psql('postgres',
	"SELECT COUNT(*) FROM pg_prepared_xacts");
is($result, "0",
	"check all prepared transactions are resolved on the subscriber");

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
