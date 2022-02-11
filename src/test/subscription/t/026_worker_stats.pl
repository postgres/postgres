
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Tests for subscription error stats.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test if the error reported on pg_stat_subscription_workers view is expected.
sub test_subscription_error
{
    my ($node, $relname, $command, $xid, $by_apply_worker, $errmsg_prefix, $msg)
	= @_;

    my $check_sql = qq[
SELECT count(1) > 0
FROM pg_stat_subscription_workers
WHERE last_error_relid = '$relname'::regclass
    AND starts_with(last_error_message, '$errmsg_prefix')];

    # subrelid
    $check_sql .= $by_apply_worker
	? qq[ AND subrelid IS NULL]
	: qq[ AND subrelid = '$relname'::regclass];

    # last_error_command
    $check_sql .= $command eq ''
	? qq[ AND last_error_command IS NULL]
	: qq[ AND last_error_command = '$command'];

    # last_error_xid
    $check_sql .= $xid eq ''
	? qq[ AND last_error_xid IS NULL]
	: qq[ AND last_error_xid = '$xid'::xid];

    # Wait for the particular error statistics to be reported.
    $node->poll_query_until('postgres', $check_sql,
) or die "Timed out while waiting for " . $msg;
}

# Create publisher node.
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node.
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');

# The subscriber will enter an infinite error loop, so we don't want
# to overflow the server log with error messages.
$node_subscriber->append_conf('postgresql.conf',
			      qq[
wal_retrieve_retry_interval = 2s
]);
$node_subscriber->start;

# Initial table setup on both publisher and subscriber. On subscriber we
# create the same tables but with primary keys. Also, insert some data that
# will conflict with the data replicated from publisher later.
$node_publisher->safe_psql(
    'postgres',
    qq[
BEGIN;
CREATE TABLE test_tab1 (a int);
CREATE TABLE test_tab2 (a int);
INSERT INTO test_tab1 VALUES (1);
INSERT INTO test_tab2 VALUES (1);
COMMIT;
]);
$node_subscriber->safe_psql(
    'postgres',
    qq[
BEGIN;
CREATE TABLE test_tab1 (a int primary key);
CREATE TABLE test_tab2 (a int primary key);
INSERT INTO test_tab2 VALUES (1);
COMMIT;
]);

# Setup publications.
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql(
    'postgres',
    "CREATE PUBLICATION tap_pub FOR TABLE test_tab1, test_tab2;");

# There shouldn't be any subscription errors before starting logical replication.
my $result = $node_subscriber->safe_psql(
    'postgres',
    "SELECT count(1) FROM pg_stat_subscription_workers");
is($result, qq(0), 'check no subscription error');

# Create subscription. The table sync for test_tab2 on tap_sub will enter into
# infinite error loop due to violating the unique constraint.
$node_subscriber->safe_psql(
    'postgres',
    "CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub;");

$node_publisher->wait_for_catchup('tap_sub');

# Wait for initial table sync for test_tab1 to finish.
$node_subscriber->poll_query_until(
    'postgres',
    qq[
SELECT count(1) = 1 FROM pg_subscription_rel
WHERE srrelid = 'test_tab1'::regclass AND srsubstate in ('r', 's')
]) or die "Timed out while waiting for subscriber to synchronize data";

# Check the initial data.
$result = $node_subscriber->safe_psql(
    'postgres',
    "SELECT count(a) FROM test_tab1");
is($result, q(1), 'check initial data are copied to subscriber');

# Insert more data to test_tab1, raising an error on the subscriber due to
# violation of the unique constraint on test_tab1.
my $xid = $node_publisher->safe_psql(
    'postgres',
    qq[
BEGIN;
INSERT INTO test_tab1 VALUES (1);
SELECT pg_current_xact_id()::xid;
COMMIT;
]);
test_subscription_error($node_subscriber, 'test_tab1', 'INSERT', $xid,
			1,	# check apply worker error
			qq(duplicate key value violates unique constraint),
			'error reported by the apply worker');

# Check the table sync worker's error in the view.
test_subscription_error($node_subscriber, 'test_tab2', '', '',
			0,	# check tablesync worker error
			qq(duplicate key value violates unique constraint),
			'the error reported by the table sync worker');

# Test for resetting subscription worker statistics.
# Truncate test_tab1 and test_tab2 so that applying changes and table sync can
# continue, respectively.
$node_subscriber->safe_psql(
    'postgres',
    "TRUNCATE test_tab1, test_tab2;");

# Wait for the data to be replicated.
$node_subscriber->poll_query_until(
    'postgres',
    "SELECT count(1) > 0 FROM test_tab1");
$node_subscriber->poll_query_until(
    'postgres',
    "SELECT count(1) > 0 FROM test_tab2");

# There shouldn't be any errors in the view after dropping the subscription.
$node_subscriber->safe_psql(
    'postgres',
    "DROP SUBSCRIPTION tap_sub;");
$result = $node_subscriber->safe_psql(
    'postgres',
    "SELECT count(1) FROM pg_stat_subscription_workers");
is($result, q(0), 'no error after dropping subscription');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
