# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a cascading standby can reconnect to its upstream standby after
# advancing past the upstream's WAL flush position via archive recovery.
#
# Setup: praline -> samurai -> stubble
# stubble has both streaming (from samurai) and restore_command
# (from praline's archive).
#
# When samurai's walreceiver is stopped and stubble falls back to
# archive recovery, stubble may advance its recovery position past
# samurai's replay position.  Previously, stubble's walreceiver
# would fail with "requested starting point is ahead of the WAL flush
# position" when reconnecting to samurai.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize praline with archiving
my $praline = PostgreSQL::Test::Cluster->new('praline');
$praline->init(allows_streaming => 1, has_archiving => 1);
$praline->append_conf(
	'postgresql.conf', qq(
wal_keep_size = 128MB
checkpoint_timeout = 1h
));
$praline->start;

# Take backup and create samurai (streaming from praline, no archive)
my $backup_name = 'my_backup';
$praline->backup($backup_name);

my $samurai = PostgreSQL::Test::Cluster->new('samurai');
$samurai->init_from_backup($praline, $backup_name, has_streaming => 1);
$samurai->start;

# Wait for samurai to start streaming
$praline->wait_for_catchup($samurai);

# Take backup from samurai and create stubble
# stubble streams from samurai AND restores from praline's archive
$samurai->backup($backup_name);

my $stubble = PostgreSQL::Test::Cluster->new('stubble');
$stubble->init_from_backup($samurai, $backup_name, has_streaming => 1);
$stubble->enable_restoring($praline);
$stubble->start;

# Generate initial data and wait for full cascade replication
$praline->safe_psql('postgres',
	"CREATE TABLE test_tab AS SELECT generate_series(1, 1000) AS id");
$praline->wait_for_replay_catchup($samurai);
$samurai->wait_for_replay_catchup($stubble, $praline);

my $result = $stubble->safe_psql('postgres', "SELECT count(*) FROM test_tab");
is($result, '1000', 'initial data replicated to stubble');

# Disconnect samurai from praline by clearing primary_conninfo.
# This stops samurai's walreceiver, so samurai can no longer receive
# new WAL.  Its GetStandbyFlushRecPtr() will return only replayPtr.
$samurai->append_conf('postgresql.conf', "primary_conninfo = ''");
$samurai->reload;

# Wait for samurai's walreceiver to stop
$samurai->poll_query_until('postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_wal_receiver)")
  or die "Timed out waiting for samurai walreceiver to stop";

# Stop stubble cleanly.  We'll restart it after generating new WAL
# so it enters the recovery state machine fresh and tries archive first.
$stubble->stop;

# Force a checkpoint now so that no background checkpoint can generate
# extra WAL during the INSERT below and push it across a segment boundary.
# Combined with checkpoint_timeout = 1h this ensures the new WAL fits
# within a single segment, keeping the gap within wal_segment_size.
$praline->safe_psql('postgres', "CHECKPOINT");

# Generate more WAL on praline
$praline->safe_psql('postgres',
	"INSERT INTO test_tab SELECT generate_series(1001, 2000)");

# Force WAL switch and wait for archiving to complete, so that
# stubble can find the new WAL in the archive when it starts.
my $walfile = $praline->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_current_wal_lsn())");
$praline->safe_psql('postgres', "SELECT pg_switch_wal()");
$praline->poll_query_until('postgres',
	"SELECT '$walfile' <= last_archived_wal FROM pg_stat_archiver")
  or die "Timed out waiting for WAL archiving";

# Rotate stubble's log so we can check just the new log output
$stubble->rotate_logfile;
my $stubble_log_offset = -s $stubble->logfile;

# Start stubble.  It will:
# 1. Read new WAL from praline's archive (XLOG_FROM_ARCHIVE)
# 2. Advance RecPtr past samurai's replay position
# 3. Try streaming from samurai (XLOG_FROM_STREAM)
# 4. detect that upstream is behind via
#    IDENTIFY_SYSTEM and wait instead of failing
$stubble->start;

# Wait for stubble to replay the new data from archive
$stubble->poll_query_until('postgres',
	"SELECT count(*) >= 2000 FROM test_tab")
  or die "Timed out waiting for stubble to replay archived WAL";

$result = $stubble->safe_psql('postgres', "SELECT count(*) FROM test_tab");
is($result, '2000', 'stubble replayed new data from archive');

# Wait for walreceiver to hit the upstream-catchup wait event, proving we
# exercised the START_REPLICATION-ahead-of-upstream path.
ok( $stubble->poll_query_until(
		'postgres',
		qq[SELECT count(*) > 0 FROM pg_stat_activity
		WHERE backend_type = 'walreceiver' AND wait_event = 'WalReceiverUpstreamCatchup'
		]),
	"walreceiver has caught up"
) or die qq(Timed out while waiting for walreceiver to catch up);

# Verify no errors occurred in stubble.
my $stubble_loglines =
  PostgreSQL::Test::Utils::slurp_file($stubble->logfile, $stubble_log_offset);
ok( $stubble_loglines !~ m/ERROR/, 'no errors in stubble log');

# Now restore samurai's streaming from praline so it can catch up
$samurai->enable_streaming($praline);
$samurai->reload;

# Wait for samurai to catch up with praline
$praline->wait_for_replay_catchup($samurai);

# stubble's walreceiver should eventually connect to samurai and
# resume streaming (once samurai has caught up past stubble's position)
$samurai->poll_query_until('postgres',
	"SELECT EXISTS (SELECT 1 FROM pg_stat_replication)")
  or die "Timed out waiting for stubble to reconnect to samurai";

# Verify end-to-end cascade streaming works with new data
$praline->safe_psql('postgres',
	"INSERT INTO test_tab SELECT generate_series(2001, 3000)");
$praline->wait_for_replay_catchup($samurai);
$samurai->wait_for_replay_catchup($stubble, $praline);

$result = $stubble->safe_psql('postgres', "SELECT count(*) FROM test_tab");
is($result, '3000',
	'cascade streaming resumes normally after upstream catches up');

done_testing();

# Backported method
package PostgreSQL::Test::Cluster;
sub wait_for_replay_catchup
{
   my ($self, $standby_name, $node) = @_;
   $node = defined($node) ? $node : $self;

   $self->wait_for_catchup($standby_name, 'replay', $node->lsn('flush'));
}
