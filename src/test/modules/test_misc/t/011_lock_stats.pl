
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test for the lock statistics
#
# This test creates multiple locking situations when a session (s2) has to
# wait on a lock for longer than deadlock_timeout. The first tests each test a
# dedicated lock type.
# The last one checks that log_lock_waits has no impact on the statistics
# counters.

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
	$s2->query_safe(
		q[
			SELECT injection_points_set_local();
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
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('$point_name');");
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('$point_name');");
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

# deadlock_timeout fired, now commit in s1 and s2
$s1->query_safe(q(COMMIT));
$s2->query_safe(q(COMMIT));

# check that pg_stat_lock has been updated
wait_for_pg_stat_lock($node, 'relation');
ok(1, "Lock stats ok for relation");

# close sessions
$s1->quit;
$s2->quit;

####### transaction lock

setup_sessions();

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

# deadlock_timeout fired, now commit in s1 and s2
$s1->query_safe(q(COMMIT));
$s2->query_safe(q(COMMIT));

# check that pg_stat_lock has been updated
wait_for_pg_stat_lock($node, 'transactionid');
ok(1, "Lock stats ok for transactionid");

# Close sessions
$s1->quit;
$s2->quit;

####### advisory lock

setup_sessions();

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

# Close sessions
$s1->quit;
$s2->quit;

####### Ensure log_lock_waits has no impact

setup_sessions();

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

# close sessions
$s1->quit;
$s2->quit;

# cleanup
$node->safe_psql('postgres', q[DROP TABLE test_stat_tab;]);

done_testing();
