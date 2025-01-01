
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test streaming of simple large transaction
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

	# Interleave a pair of transactions, each exceeding the 64kB limit.
	my $offset = 0;

	my $h = $node_publisher->background_psql('postgres', on_error_stop => 0);

	# Check the subscriber log from now on.
	$offset = -s $node_subscriber->logfile;

	$h->query_safe(
		q{
	BEGIN;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(3, 5000) s(i);
	UPDATE test_tab SET b = sha256(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	});

	$node_publisher->safe_psql(
		'postgres', q{
	BEGIN;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(5001, 9999) s(i);
	DELETE FROM test_tab WHERE a > 5000;
	COMMIT;
	});

	$h->query_safe('COMMIT');
	# errors make the next test fail, so ignore them here
	$h->quit;

	$node_publisher->wait_for_catchup($appname);

	check_parallel_log($node_subscriber, $offset, $is_parallel, 'COMMIT');

	my $result =
	  $node_subscriber->safe_psql('postgres',
		"SELECT count(*), count(c), count(d = 999) FROM test_tab");
	is($result, qq(3334|3334|3334),
		'check extra columns contain local defaults');

	# Test the streaming in binary mode
	$node_subscriber->safe_psql('postgres',
		"ALTER SUBSCRIPTION tap_sub SET (binary = on)");

	# Check the subscriber log from now on.
	$offset = -s $node_subscriber->logfile;

	# Insert, update and delete enough rows to exceed the 64kB limit.
	$node_publisher->safe_psql(
		'postgres', q{
	BEGIN;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(5001, 10000) s(i);
	UPDATE test_tab SET b = sha256(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	COMMIT;
	});

	$node_publisher->wait_for_catchup($appname);

	check_parallel_log($node_subscriber, $offset, $is_parallel, 'COMMIT');

	$result =
	  $node_subscriber->safe_psql('postgres',
		"SELECT count(*), count(c), count(d = 999) FROM test_tab");
	is($result, qq(6667|6667|6667),
		'check extra columns contain local defaults');

	# Change the local values of the extra columns on the subscriber,
	# update publisher, and check that subscriber retains the expected
	# values. This is to ensure that non-streaming transactions behave
	# properly after a streaming transaction.
	$node_subscriber->safe_psql('postgres',
		"UPDATE test_tab SET c = 'epoch'::timestamptz + 987654321 * interval '1s'"
	);

	# Check the subscriber log from now on.
	$offset = -s $node_subscriber->logfile;

	$node_publisher->safe_psql('postgres',
		"UPDATE test_tab SET b = sha256(a::text::bytea)");

	$node_publisher->wait_for_catchup($appname);

	check_parallel_log($node_subscriber, $offset, $is_parallel, 'COMMIT');

	$result = $node_subscriber->safe_psql('postgres',
		"SELECT count(*), count(extract(epoch from c) = 987654321), count(d = 999) FROM test_tab"
	);
	is($result, qq(6667|6667|6667),
		'check extra columns contain locally changed data');

	# Cleanup the test data
	$node_publisher->safe_psql('postgres',
		"DELETE FROM test_tab WHERE (a > 2)");
	$node_publisher->wait_for_catchup($appname);
}

# Create publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	'logical_decoding_work_mem = 64kB');
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

$node_publisher->safe_psql('postgres', "CREATE TABLE test_tab_2 (a int)");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b bytea, c timestamptz DEFAULT now(), d bigint DEFAULT 999)"
);

$node_subscriber->safe_psql('postgres', "CREATE TABLE test_tab_2 (a int)");
$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX idx_tab on test_tab_2(a)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE test_tab, test_tab_2");

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
	"ALTER SUBSCRIPTION tap_sub SET(streaming = parallel, binary = off)");

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

# Test that the deadlock is detected among the leader and parallel apply
# workers.

$node_subscriber->append_conf('postgresql.conf', "deadlock_timeout = 10ms");
$node_subscriber->reload;

# Run a query to make sure that the reload has taken effect.
$node_subscriber->safe_psql('postgres', q{SELECT 1});

# Interleave a pair of transactions, each exceeding the 64kB limit.
my $h = $node_publisher->background_psql('postgres', on_error_stop => 0);

# Confirm if a deadlock between the leader apply worker and the parallel apply
# worker can be detected.

my $offset = -s $node_subscriber->logfile;

$h->query_safe(
	q{
BEGIN;
INSERT INTO test_tab_2 SELECT i FROM generate_series(1, 5000) s(i);
});

# Ensure that the parallel apply worker executes the insert command before the
# leader worker.
$node_subscriber->wait_for_log(
	qr/DEBUG: ( [A-Z0-9]+:)? applied [0-9]+ changes in the streaming chunk/,
	$offset);

$node_publisher->safe_psql('postgres', "INSERT INTO test_tab_2 values(1)");

$h->query_safe('COMMIT');
$h->quit;

$node_subscriber->wait_for_log(qr/ERROR: ( [A-Z0-9]+:)? deadlock detected/,
	$offset);

# In order for the two transactions to be completed normally without causing
# conflicts due to the unique index, we temporarily drop it.
$node_subscriber->safe_psql('postgres', "DROP INDEX idx_tab");

# Wait for this streaming transaction to be applied in the apply worker.
$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM test_tab_2");
is($result, qq(5001), 'data replicated to subscriber after dropping index');

# Clean up test data from the environment.
$node_publisher->safe_psql('postgres', "TRUNCATE TABLE test_tab_2");
$node_publisher->wait_for_catchup($appname);
$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX idx_tab on test_tab_2(a)");

# Confirm if a deadlock between two parallel apply workers can be detected.

# Check the subscriber log from now on.
$offset = -s $node_subscriber->logfile;

$h->query_safe(
	q{
BEGIN;
INSERT INTO test_tab_2 SELECT i FROM generate_series(1, 5000) s(i);
});

# Ensure that the first parallel apply worker executes the insert command
# before the second one.
$node_subscriber->wait_for_log(
	qr/DEBUG: ( [A-Z0-9]+:)? applied [0-9]+ changes in the streaming chunk/,
	$offset);

$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab_2 SELECT i FROM generate_series(1, 5000) s(i)");

$h->query_safe('COMMIT');
$h->quit;

$node_subscriber->wait_for_log(qr/ERROR: ( [A-Z0-9]+:)? deadlock detected/,
	$offset);

# In order for the two transactions to be completed normally without causing
# conflicts due to the unique index, we temporarily drop it.
$node_subscriber->safe_psql('postgres', "DROP INDEX idx_tab");

# Wait for this streaming transaction to be applied in the apply worker.
$node_publisher->wait_for_catchup($appname);

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM test_tab_2");
is($result, qq(10000), 'data replicated to subscriber after dropping index');

# Test serializing changes to files and notify the parallel apply worker to
# apply them at the end of the transaction.
$node_subscriber->append_conf('postgresql.conf',
	'debug_logical_replication_streaming = immediate');
# Reset the log_min_messages to default.
$node_subscriber->append_conf('postgresql.conf',
	"log_min_messages = warning");
$node_subscriber->reload;

# Run a query to make sure that the reload has taken effect.
$node_subscriber->safe_psql('postgres', q{SELECT 1});

$offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab_2 SELECT i FROM generate_series(1, 5000) s(i)");

# Ensure that the changes are serialized.
$node_subscriber->wait_for_log(
	qr/LOG: ( [A-Z0-9]+:)? logical replication apply worker will serialize the remaining changes of remote transaction \d+ to a file/,
	$offset);

$node_publisher->wait_for_catchup($appname);

# Check that transaction is committed on subscriber
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM test_tab_2");
is($result, qq(15000),
	'parallel apply worker replayed all changes from file');

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
