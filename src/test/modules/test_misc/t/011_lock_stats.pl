
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test for the lock statistics and log_lock_waits
#
# This test creates multiple locking situations when a session (s2) has to
# wait on a lock for longer than deadlock_timeout. The first tests each test a
# dedicated lock type.
# The last one checks that log_lock_waits has no impact on the statistics
# counters.
#
# This test also checks that log_lock_waits messages are emitted both when
# a wait occurs and when the lock is acquired, and that the "still waiting for"
# message is logged exactly once per wait, even if the backend wakes due
# to signals.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $deadlock_timeout = 10;
my $s1;
my $s2;
my $node;

# Setup the 2 sessions
sub setup_sessions
{
	$s1 = $node->background_psql('postgres');
	$s2 = $node->background_psql('postgres');

	# Setup injection points for the waiting session
	$s2->query_until(
		qr/attaching_injection_point/, q[
			\echo attaching_injection_point
			SELECT injection_points_attach('deadlock-timeout-fired', 'wait');
		]);
}

# Fetch waits and wait_time from pg_stat_lock for a given lock type
# until they reached expected values: at least one wait and waiting longer
# than the deadlock_timeout.
sub wait_for_pg_stat_lock
{
	my ($node, $lock_type) = @_;

	$node->poll_query_until(
		'postgres', qq[
		SELECT waits > 0 AND wait_time >= $deadlock_timeout
		FROM pg_stat_lock
		WHERE locktype = '$lock_type';
	]) or die "Timed out waiting for pg_stat_lock for $lock_type";
}

# Convenience wrapper to wait for a point, then detach it.
sub wait_and_detach
{
	my ($node, $point_name) = @_;

	$node->wait_for_event('client backend', $point_name);
	$node->safe_psql(
		'postgres', qq[
SELECT injection_points_detach('$point_name');
SELECT injection_points_wakeup('$point_name');
]);
}

# Node initialization
$node = PostgreSQL::Test::Cluster->new('node');
$node->init();
$node->append_conf('postgresql.conf',
	"deadlock_timeout = ${deadlock_timeout}ms");
$node->start();

# Check if the extension injection_points is available
plan skip_all => 'Extension injection_points not installed'
  unless $node->check_extension('injection_points');

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

$node->safe_psql(
	'postgres', q[
CREATE TABLE test_stat_tab(key text not null, value int);
INSERT INTO test_stat_tab(key, value) VALUES('k0', 1);
]);

############################################################################

####### Relation lock

setup_sessions();

my $log_offset = -s $node->logfile;

$s1->query_safe(
	q[
SELECT pg_stat_reset_shared('lock');
BEGIN;
LOCK TABLE test_stat_tab;
]);

# s2 setup
$s2->query_safe(
	q[
BEGIN;
SELECT pg_stat_force_next_flush();
]);
# s2 blocks on LOCK.
$s2->query_until(
	qr/lock_s2/, q[
\echo lock_s2
LOCK TABLE test_stat_tab;
]);

wait_and_detach($node, 'deadlock-timeout-fired');

# Check that log_lock_waits message is emitted during a lock wait.
$node->wait_for_log(qr/still waiting for AccessExclusiveLock on relation/,
	$log_offset);

# Wake the backend waiting on the lock and confirm it woke by calling
# pg_log_backend_memory_contexts() and checking for the logged memory
# contexts. This is necessary to test later that the "still waiting for"
# message is logged exactly once per wait, even if the backend wakes
# during the wait.
$node->safe_psql(
	'postgres', q[SELECT pg_log_backend_memory_contexts(pid)
	FROM pg_locks WHERE locktype = 'relation' AND
	relation = 'test_stat_tab'::regclass AND NOT granted;]);
$node->wait_for_log(qr/logging memory contexts/, $log_offset);

# deadlock_timeout fired, now commit in s1 and s2
$s1->query_safe(q(COMMIT));
$s2->query_safe(q(COMMIT));

# check that pg_stat_lock has been updated
wait_for_pg_stat_lock($node, 'relation');
ok(1, "Lock stats ok for relation");

# Check that log_lock_waits message is emitted when the lock is acquired
# after waiting.
$node->wait_for_log(qr/acquired AccessExclusiveLock on relation/,
	$log_offset);

# Check that the "still waiting for" message is logged exactly once per wait,
# even if the backend wakes during the wait.
my $log_contents = slurp_file($node->logfile, $log_offset);
my @still_waiting = ($log_contents =~ /still waiting for/g);
is( scalar @still_waiting,
	1,
	"still waiting logged exactly once despite wakeups from pg_log_backend_memory_contexts()"
);

# close sessions
$s1->quit;
$s2->quit;

####### transaction lock

setup_sessions();

$log_offset = -s $node->logfile;

$s1->query_safe(
	q[
SELECT pg_stat_reset_shared('lock');
INSERT INTO test_stat_tab(key, value) VALUES('k1', 1), ('k2', 1), ('k3', 1);
BEGIN;
UPDATE test_stat_tab SET value = value + 1 WHERE key = 'k1';
]);

# s2 setup
$s2->query_safe(
	q[
SET log_lock_waits = on;
BEGIN;
SELECT pg_stat_force_next_flush();
]);
# s2 blocks here on UPDATE
$s2->query_until(
	qr/lock_s2/, q[
\echo lock_s2
UPDATE test_stat_tab SET value = value + 1 WHERE key = 'k1';
]);

wait_and_detach($node, 'deadlock-timeout-fired');

# Check that log_lock_waits message is emitted during a lock wait.
$node->wait_for_log(qr/still waiting for ShareLock on transaction/,
	$log_offset);

# deadlock_timeout fired, now commit in s1 and s2
$s1->query_safe(q(COMMIT));
$s2->query_safe(q(COMMIT));

# check that pg_stat_lock has been updated
wait_for_pg_stat_lock($node, 'transactionid');
ok(1, "Lock stats ok for transactionid");

# Check that log_lock_waits message is emitted when the lock is acquired
# after waiting.
$node->wait_for_log(qr/acquired ShareLock on transaction/, $log_offset);

# Close sessions
$s1->quit;
$s2->quit;

####### advisory lock

setup_sessions();

$log_offset = -s $node->logfile;

$s1->query_safe(
	q[
SELECT pg_stat_reset_shared('lock');
SELECT pg_advisory_lock(1);
]);

# s2 setup
$s2->query_safe(
	q[
SET log_lock_waits = on;
BEGIN;
SELECT pg_stat_force_next_flush();
]);
# s2 blocks on the advisory lock.
$s2->query_until(
	qr/lock_s2/, q[
\echo lock_s2
SELECT pg_advisory_lock(1);
]);

wait_and_detach($node, 'deadlock-timeout-fired');

# Check that log_lock_waits message is emitted during a lock wait.
$node->wait_for_log(qr/still waiting for ExclusiveLock on advisory lock/,
	$log_offset);

# deadlock_timeout fired, now unlock and commit s2
$s1->query_safe(q(SELECT pg_advisory_unlock(1)));
$s2->query_safe(
	q[
SELECT pg_advisory_unlock(1);
COMMIT;
]);

# check that pg_stat_lock has been updated
wait_for_pg_stat_lock($node, 'advisory');
ok(1, "Lock stats ok for advisory");

# Check that log_lock_waits message is emitted when the lock is acquired
# after waiting.
$node->wait_for_log(qr/acquired ExclusiveLock on advisory lock/, $log_offset);

# Close sessions
$s1->quit;
$s2->quit;

####### Ensure log_lock_waits has no impact

setup_sessions();

$log_offset = -s $node->logfile;

$s1->query_safe(
	q[
SELECT pg_stat_reset_shared('lock');
BEGIN;
LOCK TABLE test_stat_tab;
]);

# s2 setup
$s2->query_safe(
	q[
SET log_lock_waits = off;
BEGIN;
SELECT pg_stat_force_next_flush();
]);
# s2 blocks on LOCK.
$s2->query_until(
	qr/lock_s2/, q[
\echo lock_s2
LOCK TABLE test_stat_tab;
]);

wait_and_detach($node, 'deadlock-timeout-fired');

# deadlock_timeout fired, now commit in s1 and s2
$s1->query_safe(q(COMMIT));
$s2->query_safe(q(COMMIT));

# check that pg_stat_lock has been updated
wait_for_pg_stat_lock($node, 'relation');
ok(1, "log_lock_waits has no impact on Lock stats");

# Check that no log_lock_waits messages are emitted
ok( !$node->log_contains(
		"still waiting for AccessExclusiveLock on relation", $log_offset),
	'check that no log_lock_waits message is emitted during a lock wait');
ok( !$node->log_contains(
		"acquired AccessExclusiveLock on relation", $log_offset),
	'check that no log_lock_waits message is emitted when the lock is acquired after waiting'
);

# close sessions
$s1->quit;
$s2->quit;

# cleanup
$node->safe_psql('postgres', q[DROP TABLE test_stat_tab;]);

done_testing();
