
# Copyright (c) 2026, PostgreSQL Global Development Group

# Checks that the publisher is able to shut down without
# waiting for sending of all pending data to the subscriber
# with wal_sender_shutdown_timeout set
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf(
	'postgresql.conf',
	qq(wal_sender_timeout = 1h
	 wal_sender_shutdown_timeout = 10ms));
$node_publisher->start;

# Initialize subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Create publication for test table
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_tab (id int PRIMARY KEY);
	CREATE PUBLICATION test_pub FOR TABLE test_tab;
));

# Create matching table and subscription on subscriber
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE test_tab (id int PRIMARY KEY);
	CREATE SUBSCRIPTION test_sub CONNECTION '$publisher_connstr' PUBLICATION test_pub	WITH (failover = true);
));

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'test_sub');

# Start a background session on the subscriber to run a transaction later
# that will block the logical apply worker on a lock.
my $sub_session = $node_subscriber->background_psql('postgres');

# Test that when the logical apply worker is blocked on a lock and replication
# is stalled, shutting down the publisher causes the logical walsender to exit
# due to wal_sender_shutdown_timeout, allowing shutdown to complete.

# Cause the logical apply worker to block on a lock by running conflicting
# transactions on the publisher and subscriber.
$sub_session->query_safe("BEGIN; INSERT INTO test_tab VALUES (0);");
$node_publisher->safe_psql('postgres', "INSERT INTO test_tab VALUES (0);");

my $log_offset = -s $node_publisher->logfile;

# Verify that the walsender exits due to wal_sender_shutdown_timeout.
$node_publisher->stop('fast');
ok( $node_publisher->log_contains(
		qr/WARNING: .* terminating walsender process due to replication shutdown timeout/,
		$log_offset),
	"walsender exits due to wal_sender_shutdown_timeout");

$sub_session->query_safe("ABORT;");
$node_publisher->start;
$node_publisher->wait_for_catchup('test_sub');

# Test that when the logical apply worker is blocked on a lock, replication
# is stalled, and the logical walsender's output buffer is full, shutting down
# the publisher causes the walsender to exit due to
# wal_sender_shutdown_timeout, allowing shutdown to complete.
#
# This test differs from the previous one in that the walsender's output
# buffer is full (because pending data cannot be transferred).

# Run a transaction on the subscriber that blocks the logical apply worker
# on a lock.
$sub_session->query_safe("BEGIN; LOCK TABLE test_tab IN EXCLUSIVE MODE;");

# Generate enough data to fill the logical walsender's output buffer.
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab VALUES (generate_series(1, 20000));");

# Wait for the logical walsender's output buffer to fill. If the WAL send
# positions do not advance between checks, treat the buffer as full.
my $last_sent_lsn = $node_publisher->safe_psql('postgres',
	"SELECT sent_lsn FROM pg_stat_replication WHERE application_name = 'test_sub';"
);

my $max_attempts = $PostgreSQL::Test::Utils::timeout_default * 10;
while ($max_attempts-- >= 0)
{
	usleep(100_000);

	my $cur_sent_lsn = $node_publisher->safe_psql('postgres',
		"SELECT sent_lsn FROM pg_stat_replication WHERE application_name = 'test_sub';"
	);

	my $diff = $node_publisher->safe_psql('postgres',
		"SELECT pg_wal_lsn_diff('$cur_sent_lsn', '$last_sent_lsn');");
	last if $diff == 0;

	$last_sent_lsn = $cur_sent_lsn;
}

$log_offset = -s $node_publisher->logfile;

# Verify that the walsender exits due to wal_sender_shutdown_timeout.
$node_publisher->stop('fast');
ok( $node_publisher->log_contains(
		qr/WARNING: .* terminating walsender process due to replication shutdown timeout/,
		$log_offset),
	"walsender with full output buffer exits due to wal_sender_shutdown_timeout"
);

$sub_session->query_safe("ABORT;");

# The next test depends on Perl's `kill`, which apparently is not
# portable to Windows.  (It would be nice to use Test::More's `subtest`,
# but that's not in the ancient version we require.)
if ($PostgreSQL::Test::Utils::windows_os)
{
	$node_subscriber->stop('fast');
	done_testing();
	exit;
}

$node_publisher->start;

# Test that wal_sender_shutdown_timeout works correctly when both physical
# and logical replication are active, and slot synchronization is running on
# the standby.
#
# In this scenario, the logical apply worker is blocked on a lock and
# the standby's walreceiver is stopped (via SIGSTOP signal), stalling both
# replication streams. Verify that shutting down the publisher (primary)
# causes both physical and logical walsenders to exit due to
# wal_sender_shutdown_timeout, allowing shutdown to complete.
#
# Skip this test on Windows.

# Create the standby with slot synchronization enabled.
$node_publisher->backup(
	'publisher_backup',
	backup_options => [
		'--create-slot', '--slot',
		'test_slot', '-d',
		'dbname=postgres', '--write-recovery-conf'
	]);

$node_publisher->append_conf('postgresql.conf',
	"synchronized_standby_slots = 'test_slot'");
$node_publisher->reload;

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_publisher, 'publisher_backup');
$node_standby->append_conf(
	'postgresql.conf',
	qq(sync_replication_slots = on
	hot_standby_feedback = on));
$node_standby->start;

# Cause the logical apply worker to block on a lock by running conflicting
# transactions on the publisher and subscriber, stalling logical replication.
$node_publisher->wait_for_catchup('test_sub');
$sub_session->query_safe("BEGIN; LOCK TABLE test_tab IN EXCLUSIVE MODE;");
$node_publisher->safe_psql('postgres', "INSERT INTO test_tab VALUES (-1); ");

# Cause the standby's walreceiver to be blocked with SIGSTOP signal,
# stalling physical replication.
$node_standby->poll_query_until('postgres',
	"SELECT EXISTS(SELECT 1 FROM pg_stat_wal_receiver)");
my $receiverpid = $node_standby->safe_psql('postgres',
	"SELECT pid FROM pg_stat_wal_receiver");
like($receiverpid, qr/^[0-9]+$/, "have walreceiver pid $receiverpid");
kill 'STOP', $receiverpid;

$log_offset = -s $node_publisher->logfile;

# Verify that the walsender exits due to wal_sender_shutdown_timeout
# even when both physical and logical replication are stalled.
# wal_sender_shutdown_timeout.
$node_publisher->safe_psql('postgres', "INSERT INTO test_tab VALUES (-2);");
$node_publisher->stop('fast');
ok( $node_publisher->log_contains(
		qr/WARNING: .* terminating walsender process due to replication shutdown timeout/,
		$log_offset),
	"walsender exits due to wal_sender_shutdown_timeout even when both physical and logical replication are stalled"
);

kill 'CONT', $receiverpid;
$sub_session->quit;

$node_subscriber->stop('fast');
$node_standby->stop('fast');

done_testing();
