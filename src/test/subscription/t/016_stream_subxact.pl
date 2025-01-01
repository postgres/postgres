
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test streaming of transaction containing subtransactions
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Check that the parallel apply worker has finished applying the streaming
# transaction.
sub check_parallel_log
{
	my ($node_subscriber, $offset, $is_parallel, $type) = @_;

	if ($is_parallel)
	{
		$node_subscriber->wait_for_log(
			qr/DEBUG: ( [A-Z0-9]+:)? finished processing the STREAM $type command/,
			$offset);
	}
}

# Common test steps for both the streaming=on and streaming=parallel cases.
sub test_streaming
{
	my ($node_publisher, $node_subscriber, $appname, $is_parallel) = @_;

	my $offset = 0;

	# Check the subscriber log from now on.
	$offset = -s $node_subscriber->logfile;

	# Insert, update and delete some rows.
	$node_publisher->safe_psql(
		'postgres', q{
	BEGIN;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(3, 5) s(i);
	UPDATE test_tab SET b = sha256(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	SAVEPOINT s1;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(6, 8) s(i);
	UPDATE test_tab SET b = sha256(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	SAVEPOINT s2;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(9, 11) s(i);
	UPDATE test_tab SET b = sha256(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	SAVEPOINT s3;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(12, 14) s(i);
	UPDATE test_tab SET b = sha256(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	SAVEPOINT s4;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(15, 17) s(i);
	UPDATE test_tab SET b = sha256(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	COMMIT;
	});

	$node_publisher->wait_for_catchup($appname);

	check_parallel_log($node_subscriber, $offset, $is_parallel, 'COMMIT');

	my $result =
	  $node_subscriber->safe_psql('postgres',
		"SELECT count(*), count(c), count(d = 999) FROM test_tab");
	is($result, qq(12|12|12),
		'check data was copied to subscriber in streaming mode and extra columns contain local defaults'
	);

	# Cleanup the test data
	$node_publisher->safe_psql('postgres',
		"DELETE FROM test_tab WHERE (a > 2)");
	$node_publisher->wait_for_catchup($appname);
}

# Create publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	'debug_logical_replication_streaming = immediate');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Create some preexisting content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b bytea)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab VALUES (1, 'foo'), (2, 'bar')");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b bytea, c timestamptz DEFAULT now(), d bigint DEFAULT 999)"
);

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE test_tab");

my $appname = 'tap_sub';

################################
# Test using streaming mode 'on'
################################
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub WITH (streaming = on)"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

my $result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(2|2|2), 'check initial data was copied to subscriber');

test_streaming($node_publisher, $node_subscriber, $appname, 0);

######################################
# Test using streaming mode 'parallel'
######################################
my $oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = '$appname' AND state = 'streaming';"
);

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub SET(streaming = parallel)");

$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = '$appname' AND state = 'streaming';"
  )
  or die
  "Timed out while waiting for apply to restart after changing SUBSCRIPTION";

# We need to check DEBUG logs to ensure that the parallel apply worker has
# applied the transaction. So, bump up the log verbosity.
$node_subscriber->append_conf('postgresql.conf', "log_min_messages = debug1");
$node_subscriber->reload;

# Run a query to make sure that the reload has taken effect.
$node_subscriber->safe_psql('postgres', q{SELECT 1});

test_streaming($node_publisher, $node_subscriber, $appname, 1);

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
