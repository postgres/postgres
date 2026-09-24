# Copyright (c) 2026, PostgreSQL Global Development Group

# The snapshot of CREATE_REPLICATION_SLOT ... USE_SNAPSHOT must not be handed
# out while a transaction it takes as committed is still between writing its
# commit record and removing itself from procarray.  Checks that the slot
# creation waits for such a transaction, and that neither the slot's snapshot
# nor a later session loses the transaction's changes.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 'logical');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', q(CREATE EXTENSION injection_points));
$node->safe_psql(
	'postgres',
	q(CREATE TABLE tab(i int PRIMARY KEY, j int);
	  INSERT INTO tab VALUES (1, 1), (2, 2)));

# Wait until the given backend waits on the lock of the given transaction.
sub wait_for_xact_lock_wait
{
	my ($pid, $xid, $what) = @_;

	$node->poll_query_until('postgres',
		"SELECT count(*) > 0 FROM pg_locks WHERE pid = $pid AND locktype = 'transactionid' AND transactionid = '$xid' AND NOT granted"
	) or die "$what did not wait on the lock of transaction $xid";
}

my $s2 = $node->background_psql('postgres');
my $s3 = $node->background_psql('postgres');
my $s4 = $node->background_psql('postgres');
my $walsender = $node->background_psql('postgres', replication => 'database');

my $walsender_pid = $walsender->query_safe('SELECT pg_backend_pid()');
my $s4_pid = $s4->query_safe('SELECT pg_backend_pid()');

# s2 and s3 hold transactions with an XID, so that the snapshot builder goes
# through its BUILDING_SNAPSHOT and FULL_SNAPSHOT states rather than becoming
# consistent right away.  The walsender waits for s2, then for s3.
my $s2_xid = $s2->query_safe('BEGIN; SELECT pg_current_xact_id()');

$walsender->query_until(
	qr/started/, q(\echo started
BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ;
CREATE_REPLICATION_SLOT slot_race TEMPORARY LOGICAL test_decoding USE_SNAPSHOT;
));
wait_for_xact_lock_wait($walsender_pid, $s2_xid, 'walsender');

my $s3_xid = $s3->query_safe('BEGIN; SELECT pg_current_xact_id()');
$s2->query_safe('ROLLBACK');
wait_for_xact_lock_wait($walsender_pid, $s3_xid, 'walsender');

# While the walsender waits for s3, s4 changes the table and stops after
# writing its commit record, before updating CLOG.
$s4->query_safe(
	q(SELECT injection_points_set_local();
	  SELECT injection_points_attach('commit-before-clog-update', 'wait')));
$s4->query_until(
	qr/started/, q(\echo started
BEGIN;
INSERT INTO tab VALUES (3, 3);
UPDATE tab SET j = j + 1 WHERE i = 1;
DELETE FROM tab WHERE i = 2;
COMMIT;
));
$node->poll_query_until('postgres',
	"SELECT wait_event = 'commit-before-clog-update' FROM pg_stat_activity WHERE pid = $s4_pid"
) or die "s4 did not reach the injection point";
my $s4_xid = $node->safe_psql('postgres',
	"SELECT backend_xid FROM pg_stat_activity WHERE pid = $s4_pid");

# Now the walsender decodes s4's commit record and reaches a consistent
# state.  It must wait for s4 rather than build the snapshot.
$s3->query_safe('ROLLBACK');
my $state;
for (my $i = 0; $i < 10 * $PostgreSQL::Test::Utils::timeout_default; $i++)
{
	$state = $node->safe_psql(
		'postgres',
		"SELECT CASE WHEN a.state = 'idle in transaction' THEN 'slot created'
		             WHEN l.pid IS NOT NULL THEN 'waiting for s4' END
		 FROM pg_stat_activity a
		   LEFT JOIN pg_locks l ON l.pid = a.pid AND l.locktype = 'transactionid'
		     AND l.transactionid = '$s4_xid' AND NOT l.granted
		 WHERE a.pid = $walsender_pid");
	last if $state ne '';
	usleep(100_000);
}
is( $state,
	'waiting for s4',
	'slot creation waits for the transaction that has not updated CLOG');

# If the slot got created without waiting, use its snapshot right away:
# the scan takes s4 as aborted and sets hint bits accordingly, which is
# what the last two checks then report.
if ($state eq 'slot created')
{
	$walsender->query_until(qr/test_decoding/, '');
	diag("rows seen through the slot's snapshot before s4 updated CLOG: "
		  . $walsender->query_safe('SELECT i, j FROM tab ORDER BY i'));
}

$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('commit-before-clog-update')");
$s4->quit;

$node->poll_query_until('postgres',
	"SELECT state = 'idle in transaction' FROM pg_stat_activity WHERE pid = $walsender_pid"
) or die "slot creation did not finish";
# drain the result of CREATE_REPLICATION_SLOT
$walsender->query_until(qr/test_decoding/, '')
  if $state ne 'slot created';

is($walsender->query_safe('SELECT i, j FROM tab ORDER BY i'),
	"1|2\n3|3", "the slot's snapshot sees the transaction's changes");
is($node->safe_psql('postgres', 'SELECT i, j FROM tab ORDER BY i'),
	"1|2\n3|3", "a new session sees the transaction's changes");

is( $walsender->query_safe(
		q(WITH s AS (SELECT pg_current_snapshot() AS snap)
                 SELECT count(*) FROM s, LATERAL pg_snapshot_xip(snap) AS x(xid)
                 WHERE xid < pg_snapshot_xmin(snap) OR xid >= pg_snapshot_xmax(snap))
	),
	'0',
	'snapshot XIDs are all within xmin and xmax');

$walsender->query_safe('ROLLBACK');
$walsender->quit;
$s2->quit;
$s3->quit;
$node->stop;

done_testing();
