# Copyright (c) 2026, PostgreSQL Global Development Group

# A standby stopped after replaying an online enable, but before the
# checkpoint record that follows it on the primary.
#
# The shutdown restartpoint has no new checkpoint record to work from
# and is skipped, so the control file would keep the in-progress state
# the transition passed through, even though replay left the node at
# "on" and rewrote every page.  pg_checksums reads that field and
# refuses to run on an in-progress state, so the shutdown has to flush
# and catch it up instead.
#
# The caught-up control file then carries the watermark of the "on"
# record, which the second half of this test relies on: an offline
# disable made while the standby is down must survive the re-replay of
# that record on the next startup, or the change would be silently
# reverted.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# This test suite is expensive to execute, require PG_TEST_EXTRA to contain
# 'checksum' to run it.
if ($ENV{PG_TEST_EXTRA})
{
	plan skip_all => 'Expensive data checksums test disabled'
	  unless ($ENV{PG_TEST_EXTRA} =~ /\bchecksum(_extended)?\b/);
}
else
{
	plan skip_all => 'Expensive data checksums test disabled';
}

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1, no_data_checksums => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
wal_keep_size = 1GB
));
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
$primary->safe_psql('postgres',
	'CREATE TABLE t AS SELECT generate_series(1,10000) AS a;');

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq(
checkpoint_timeout = 1h
max_wal_size = 10GB
));
$standby->start;
$primary->wait_for_catchup($standby);

# Anchor the standby's last restartpoint here, so that nothing replayed
# from now on gives the shutdown restartpoint a newer checkpoint record
# to use.
$primary->safe_psql('postgres', 'CHECKPOINT;');
$primary->wait_for_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT;');

# Hold the enable right after the state change record, before the
# checkpoint it requests once the transition is complete.
$primary->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-on-before-checkpoint','wait');"
);
enable_data_checksums($primary);
$primary->wait_for_event('datachecksums launcher',
	'datachecksums-on-before-checkpoint');
wait_for_checksum_state($primary, 'on');

# The state change record is already flushed and streams on its own;
# no checkpoint record follows while the launcher is held.
$primary->wait_for_catchup($standby);
wait_for_checksum_state($standby, 'on');

$standby->stop('fast');

my ($ctl) = run_command([ 'pg_controldata', $standby->data_dir ]);
my ($state) = $ctl =~ /Data page checksum version:\s+(\d+)/;
is($state, '1',
	'shutdown catches the control file up with the replayed state');

command_ok([ 'pg_checksums', '--check', '-D', $standby->data_dir ],
	'pg_checksums verifies the stopped standby');

# Disable checksums offline while the standby is down.  The restart
# below resumes replay under the last restartpoint, re-reading the "on"
# record; the watermark persisted with the catch-up above marks it as
# already applied, so the offline change must survive.
$standby->checksum_disable_offline;

# Let the enable finish on the primary.
$primary->safe_psql('postgres',
	"SELECT injection_points_wakeup('datachecksums-on-before-checkpoint');");
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksums-on-before-checkpoint');");

# The launcher exits only after the checkpoint it requests completes,
# so a checkpoint record now follows the transition in WAL.
$primary->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_catalog.pg_stat_activity "
	  . "WHERE backend_type = 'datachecksums launcher';");

my $log_offset = -s $standby->logfile;
$standby->start;
$primary->wait_for_catchup($standby);

test_checksum_state($standby, 'off');

# The nodes legitimately diverged, which replayed checkpoints report.
$standby->wait_for_log(
	qr/data checksum state "off" of this node does not match the state "on" in the replayed WAL/,
	$log_offset);

$standby->stop;
$primary->stop;

done_testing();
