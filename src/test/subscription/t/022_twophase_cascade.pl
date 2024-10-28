
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test cascading logical replication of 2PC.
#
# Includes tests for options 2PC (not-streaming) and also for 2PC (streaming).
#
# Two-phase and parallel apply will be tested in 023_twophase_stream, so we
# didn't add a parallel apply version for the tests in this file.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

###############################
# Setup a cascade of pub/sub nodes.
# node_A -> node_B -> node_C
###############################

# Initialize nodes
# node_A
my $node_A = PostgreSQL::Test::Cluster->new('node_A');
$node_A->init(allows_streaming => 'logical');
$node_A->append_conf(
	'postgresql.conf', qq(
max_prepared_transactions = 10
logical_decoding_work_mem = 64kB
));
$node_A->start;
# node_B
my $node_B = PostgreSQL::Test::Cluster->new('node_B');
$node_B->init(allows_streaming => 'logical');
$node_B->append_conf(
	'postgresql.conf', qq(
max_prepared_transactions = 10
logical_decoding_work_mem = 64kB
));
$node_B->start;
# node_C
my $node_C = PostgreSQL::Test::Cluster->new('node_C');
$node_C->init;
$node_C->append_conf(
	'postgresql.conf', qq(
max_prepared_transactions = 10
logical_decoding_work_mem = 64kB
));
$node_C->start;

# Create some pre-existing content on node_A
$node_A->safe_psql('postgres', "CREATE TABLE tab_full (a int PRIMARY KEY)");
$node_A->safe_psql(
	'postgres', "
	INSERT INTO tab_full SELECT generate_series(1,10);");

# Create the same tables on node_B and node_C
$node_B->safe_psql('postgres', "CREATE TABLE tab_full (a int PRIMARY KEY)");
$node_C->safe_psql('postgres', "CREATE TABLE tab_full (a int PRIMARY KEY)");

# Create some pre-existing content on node_A (for streaming tests)
$node_A->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b bytea)");
$node_A->safe_psql('postgres',
	"INSERT INTO test_tab VALUES (1, 'foo'), (2, 'bar')");

# Create the same tables on node_B and node_C
# columns a and b are compatible with same table name on node_A
$node_B->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b bytea, c timestamptz DEFAULT now(), d bigint DEFAULT 999)"
);
$node_C->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b bytea, c timestamptz DEFAULT now(), d bigint DEFAULT 999)"
);

# Setup logical replication

# -----------------------
# 2PC NON-STREAMING TESTS
# -----------------------

# node_A (pub) -> node_B (sub)
my $node_A_connstr = $node_A->connstr . ' dbname=postgres';
$node_A->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_A FOR TABLE tab_full, test_tab");
my $appname_B = 'tap_sub_B';
$node_B->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION tap_sub_B
	CONNECTION '$node_A_connstr application_name=$appname_B'
	PUBLICATION tap_pub_A
	WITH (two_phase = on, streaming = off)");

# node_B (pub) -> node_C (sub)
my $node_B_connstr = $node_B->connstr . ' dbname=postgres';
$node_B->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_B FOR TABLE tab_full, test_tab");
my $appname_C = 'tap_sub_C';
$node_C->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION tap_sub_C
	CONNECTION '$node_B_connstr application_name=$appname_C'
	PUBLICATION tap_pub_B
	WITH (two_phase = on, streaming = off)");

# Wait for subscribers to finish initialization
$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# Also wait for two-phase to be enabled
my $twophase_query =
  "SELECT count(1) = 0 FROM pg_subscription WHERE subtwophasestate NOT IN ('e');";
$node_B->poll_query_until('postgres', $twophase_query)
  or die "Timed out while waiting for subscriber to enable twophase";
$node_C->poll_query_until('postgres', $twophase_query)
  or die "Timed out while waiting for subscriber to enable twophase";

is(1, 1, "Cascade setup is complete");

my $result;

###############################
# check that 2PC gets replicated to subscriber(s)
# then COMMIT PREPARED
###############################

# 2PC PREPARE
$node_A->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO tab_full VALUES (11);
	PREPARE TRANSACTION 'test_prepared_tab_full';");

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check the transaction state is prepared on subscriber(s)
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber C');

# 2PC COMMIT
$node_A->safe_psql('postgres', "COMMIT PREPARED 'test_prepared_tab_full';");

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check that transaction was committed on subscriber(s)
$result = $node_B->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a = 11;");
is($result, qq(1), 'Row inserted via 2PC has committed on subscriber B');
$result = $node_C->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a = 11;");
is($result, qq(1), 'Row inserted via 2PC has committed on subscriber C');

# check the transaction state is ended on subscriber(s)
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is committed on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is committed on subscriber C');

###############################
# check that 2PC gets replicated to subscriber(s)
# then ROLLBACK PREPARED
###############################

# 2PC PREPARE
$node_A->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO tab_full VALUES (12);
	PREPARE TRANSACTION 'test_prepared_tab_full';");

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check the transaction state is prepared on subscriber(s)
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber C');

# 2PC ROLLBACK
$node_A->safe_psql('postgres', "ROLLBACK PREPARED 'test_prepared_tab_full';");

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check that transaction is aborted on subscriber(s)
$result = $node_B->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a = 12;");
is($result, qq(0), 'Row inserted via 2PC is not present on subscriber B');
$result = $node_C->safe_psql('postgres',
	"SELECT count(*) FROM tab_full where a = 12;");
is($result, qq(0), 'Row inserted via 2PC is not present on subscriber C');

# check the transaction state is ended on subscriber(s)
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is ended on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is ended on subscriber C');

###############################
# Test nested transactions with 2PC
###############################

# 2PC PREPARE with a nested ROLLBACK TO SAVEPOINT
$node_A->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO tab_full VALUES (21);
	SAVEPOINT sp_inner;
	INSERT INTO tab_full VALUES (22);
	ROLLBACK TO SAVEPOINT sp_inner;
	PREPARE TRANSACTION 'outer';
	");

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check the transaction state prepared on subscriber(s)
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber C');

# 2PC COMMIT
$node_A->safe_psql('postgres', "COMMIT PREPARED 'outer';");

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check the transaction state is ended on subscriber
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is ended on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is ended on subscriber C');

# check inserts are visible at subscriber(s).
# 22 should be rolled back.
# 21 should be committed.
$result = $node_B->safe_psql('postgres',
	"SELECT a FROM tab_full where a IN (21,22);");
is($result, qq(21), 'Rows committed are present on subscriber B');
$result = $node_C->safe_psql('postgres',
	"SELECT a FROM tab_full where a IN (21,22);");
is($result, qq(21), 'Rows committed are present on subscriber C');

# ---------------------
# 2PC + STREAMING TESTS
# ---------------------

my $oldpid_B = $node_A->safe_psql(
	'postgres', "
	SELECT pid FROM pg_stat_replication
	WHERE application_name = '$appname_B' AND state = 'streaming';");
my $oldpid_C = $node_B->safe_psql(
	'postgres', "
	SELECT pid FROM pg_stat_replication
	WHERE application_name = '$appname_C' AND state = 'streaming';");

# Setup logical replication (streaming = on)

$node_B->safe_psql(
	'postgres', "
	ALTER SUBSCRIPTION tap_sub_B
	SET (streaming = on);");
$node_C->safe_psql(
	'postgres', "
	ALTER SUBSCRIPTION tap_sub_C
	SET (streaming = on)");

# Wait for subscribers to finish initialization

$node_A->poll_query_until(
	'postgres', "
	SELECT pid != $oldpid_B FROM pg_stat_replication
	WHERE application_name = '$appname_B' AND state = 'streaming';"
) or die "Timed out while waiting for apply to restart";
$node_B->poll_query_until(
	'postgres', "
	SELECT pid != $oldpid_C FROM pg_stat_replication
	WHERE application_name = '$appname_C' AND state = 'streaming';"
) or die "Timed out while waiting for apply to restart";

###############################
# Test 2PC PREPARE / COMMIT PREPARED.
# 1. Data is streamed as a 2PC transaction.
# 2. Then do commit prepared.
#
# Expect all data is replicated on subscriber(s) after the commit.
###############################

# Insert, update and delete enough rows to exceed the 64kB limit.
# Then 2PC PREPARE
$node_A->safe_psql(
	'postgres', q{
	BEGIN;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(3, 5000) s(i);
	UPDATE test_tab SET b =  sha256(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	PREPARE TRANSACTION 'test_prepared_tab';});

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check the transaction state is prepared on subscriber(s)
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber C');

# 2PC COMMIT
$node_A->safe_psql('postgres', "COMMIT PREPARED 'test_prepared_tab';");

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check that transaction was committed on subscriber(s)
$result = $node_B->safe_psql('postgres',
	"SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(3334|3334|3334),
	'Rows inserted by 2PC have committed on subscriber B, and extra columns have local defaults'
);
$result = $node_C->safe_psql('postgres',
	"SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(3334|3334|3334),
	'Rows inserted by 2PC have committed on subscriber C, and extra columns have local defaults'
);

# check the transaction state is ended on subscriber(s)
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is committed on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is committed on subscriber C');

###############################
# Test 2PC PREPARE with a nested ROLLBACK TO SAVEPOINT.
# 0. Cleanup from previous test leaving only 2 rows.
# 1. Insert one more row.
# 2. Record a SAVEPOINT.
# 3. Data is streamed using 2PC.
# 4. Do rollback to SAVEPOINT prior to the streamed inserts.
# 5. Then COMMIT PREPARED.
#
# Expect data after the SAVEPOINT is aborted leaving only 3 rows (= 2 original + 1 from step 1).
###############################

# First, delete the data except for 2 rows (delete will be replicated)
$node_A->safe_psql('postgres', "DELETE FROM test_tab WHERE a > 2;");

# 2PC PREPARE with a nested ROLLBACK TO SAVEPOINT
$node_A->safe_psql(
	'postgres', "
	BEGIN;
	INSERT INTO test_tab VALUES (9999, 'foobar');
	SAVEPOINT sp_inner;
	INSERT INTO test_tab SELECT i, sha256(i::text::bytea) FROM generate_series(3, 5000) s(i);
	UPDATE test_tab SET b = sha256(b) WHERE mod(a,2) = 0;
	DELETE FROM test_tab WHERE mod(a,3) = 0;
	ROLLBACK TO SAVEPOINT sp_inner;
	PREPARE TRANSACTION 'outer';
	");

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check the transaction state prepared on subscriber(s)
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(1), 'transaction is prepared on subscriber C');

# 2PC COMMIT
$node_A->safe_psql('postgres', "COMMIT PREPARED 'outer';");

$node_A->wait_for_catchup($appname_B);
$node_B->wait_for_catchup($appname_C);

# check the transaction state is ended on subscriber
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is ended on subscriber B');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_prepared_xacts;");
is($result, qq(0), 'transaction is ended on subscriber C');

# check inserts are visible at subscriber(s).
# All the streamed data (prior to the SAVEPOINT) should be rolled back.
# (9999, 'foobar') should be committed.
$result = $node_B->safe_psql('postgres',
	"SELECT count(*) FROM test_tab where b = 'foobar';");
is($result, qq(1), 'Rows committed are present on subscriber B');
$result = $node_B->safe_psql('postgres', "SELECT count(*) FROM test_tab;");
is($result, qq(3), 'Rows committed are present on subscriber B');
$result = $node_C->safe_psql('postgres',
	"SELECT count(*) FROM test_tab where b = 'foobar';");
is($result, qq(1), 'Rows committed are present on subscriber C');
$result = $node_C->safe_psql('postgres', "SELECT count(*) FROM test_tab;");
is($result, qq(3), 'Rows committed are present on subscriber C');

###############################
# check all the cleanup
###############################

# cleanup the node_B => node_C pub/sub
$node_C->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_C");
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_subscription");
is($result, qq(0), 'check subscription was dropped on subscriber node C');
$result =
  $node_C->safe_psql('postgres', "SELECT count(*) FROM pg_subscription_rel");
is($result, qq(0),
	'check subscription relation status was dropped on subscriber node C');
$result = $node_C->safe_psql('postgres',
	"SELECT count(*) FROM pg_replication_origin");
is($result, qq(0),
	'check replication origin was dropped on subscriber node C');
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_replication_slots");
is($result, qq(0), 'check replication slot was dropped on publisher node B');

# cleanup the node_A => node_B pub/sub
$node_B->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_B");
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_subscription");
is($result, qq(0), 'check subscription was dropped on subscriber node B');
$result =
  $node_B->safe_psql('postgres', "SELECT count(*) FROM pg_subscription_rel");
is($result, qq(0),
	'check subscription relation status was dropped on subscriber node B');
$result = $node_B->safe_psql('postgres',
	"SELECT count(*) FROM pg_replication_origin");
is($result, qq(0),
	'check replication origin was dropped on subscriber node B');
$result =
  $node_A->safe_psql('postgres', "SELECT count(*) FROM pg_replication_slots");
is($result, qq(0), 'check replication slot was dropped on publisher node A');

# shutdown
$node_C->stop('fast');
$node_B->stop('fast');
$node_A->stop('fast');

done_testing();
