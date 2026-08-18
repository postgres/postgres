
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a standby does not become available for base backups before it
# has re-replayed the latest data checksum state change after a restart.
#
# When a standby replays XLOG2_CHECKSUMS it writes the new state to
# pg_control, but the location of the change is only tracked in shared
# memory.  If the standby restarts before a restartpoint covers the record,
# and reaches consistency below it, base backups would resume checksum
# verification with the change location unknown, while the pages rewritten
# before the change may not have reached disk.  The redo routine must
# therefore advance minRecoveryPoint to the record, so that consistency (and
# with it hot standby and base backups) is withheld until the change location
# is known again.
#
# The test holds the enabling on the primary between the state change and its
# final checkpoint, waits for the standby to replay the state change, and
# crashes the standby so that the rewritten pages never reach its disk.  WAL
# from the enabling onwards is removed from the standby's pg_wal and
# streaming is disabled, so that replay after the restart stalls below the
# state change record.  Restarted this way, the standby must refuse
# connections; once streaming is re-enabled and the record replayed again,
# base backups must succeed without spurious checksum failures.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use File::Path qw(rmtree);
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# This test suite is potentially expensive due to it requiring an increased
# shared_buffers setting and use custom timeouts.  It requires the
# "checksum_extended" PG_TEST_EXTRA setting to not cause false positives on
# slow or constrained systems.
if ($ENV{PG_TEST_EXTRA})
{
	plan skip_all => 'Expensive data checksums test disabled'
	  unless ($ENV{PG_TEST_EXTRA} =~ /\bchecksum_extended\b/);
}
else
{
	plan skip_all => 'Expensive data checksums test disabled';
}

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node_primary = PostgreSQL::Test::Cluster->new('straddle_primary');
$node_primary->init(no_data_checksums => 1, allows_streaming => 1);
# The pages rewritten while enabling must stay dirty in shared buffers until
# the final checkpoint, and their standby copies must stay dirty as well, so
# that the on-disk pages legitimately lack checksums.  wal_log_hints (implied
# by allows_streaming) is turned off to keep the page LSNs put during setup.
$node_primary->append_conf('postgresql.conf', 'shared_buffers = 128MB');
$node_primary->append_conf('postgresql.conf', 'autovacuum = off');
$node_primary->append_conf('postgresql.conf', 'wal_log_hints = off');
$node_primary->start;

$node_primary->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

my $slotname = 'physical_slot';
$node_primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('$slotname');");

# A body of relation pages whose standby copies will lack checksums
$node_primary->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,100000) AS a;");

my $backup_name = 'straddle_backup';
$node_primary->backup($backup_name);

my $node_standby = PostgreSQL::Test::Cluster->new('straddle_standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
# The background writer must not flush the replayed page rewrites behind our
# back, or nothing is left to protect and minRecoveryPoint could move on its
# own.
$node_standby->append_conf(
	'postgresql.conf', qq[
primary_slot_name = '$slotname'
bgwriter_lru_maxpages = 0
]);
$node_standby->start;

$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));
test_checksum_state($node_primary, 'off');
test_checksum_state($node_standby, 'off');

# Pin the restartpoint the later base backups will start from: replay a
# primary checkpoint and force a restartpoint on it.  No further checkpoint
# record reaches the standby until the enabling is released, so this remains
# the standby's backup starting checkpoint throughout.
$node_primary->safe_psql('postgres', 'CHECKPOINT;');
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));
$node_standby->safe_psql('postgres', 'CHECKPOINT;');

# Put everything the enabling writes into fresh WAL segments, so that the
# standby's copies of them can be removed later, and remember where the
# enabling era begins.
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal();');

# Enable checksums, holding the launcher after the state change but before
# the final checkpoint, so the rewritten pages stay dirty everywhere.
$node_primary->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-enable-checksums-delay', 'wait');"
);
$node_primary->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksums-on-before-checkpoint','wait');"
);
enable_data_checksums($node_primary);

$node_primary->wait_for_event('datachecksums launcher',
	'datachecksums-enable-checksums-delay');
my $enable_start_lsn =
  $node_primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn();');
my $enable_start_seg = $node_primary->safe_psql('postgres',
	"SELECT pg_walfile_name('$enable_start_lsn');");
$node_primary->safe_psql('postgres',
	"SELECT injection_points_wakeup('datachecksums-enable-checksums-delay');"
);

# Immediately start to wait for the next event and hold off on detaching the
# previous injection point till later to avoid delays and risk missing the
# wait event
$node_primary->wait_for_event('datachecksums launcher',
	'datachecksums-on-before-checkpoint');
# Detach the injection point now that we have some more time
$node_primary->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksums-enable-checksums-delay');"
);

# The standby has now replayed the state change: its pg_control says "on"
# while the rewritten pages are only dirty in its shared buffers.
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));
wait_for_checksum_state($node_standby, 'on');

# Crash the standby, losing the dirty rewritten pages.
$node_standby->stop('immediate');

# The state change record must have dragged minRecoveryPoint along with it,
# otherwise the standby can become consistent below the record after the
# restart.
my ($stdout, $stderr) =
  run_command([ 'pg_controldata', $node_standby->data_dir ]);
my ($min_recovery) =
  $stdout =~ /Minimum recovery ending location:\s*([0-9A-F]+\/[0-9A-F]+)/;
die "could not parse pg_controldata output" unless defined $min_recovery;

my $result = $node_primary->safe_psql('postgres',
	"SELECT '$min_recovery'::pg_lsn > '$enable_start_lsn'::pg_lsn;");
is($result, 't', 'minRecoveryPoint advanced past the checksum state change');

# Remove the enabling-era WAL from the standby and cut it off from the
# primary, so that replay after the restart stalls below the state change.
# The replication slot retains the removed segments on the primary.
my $wal_dir = $node_standby->data_dir . '/pg_wal';
opendir(my $dh, $wal_dir) or die "could not open $wal_dir: $!";
foreach my $segment (readdir($dh))
{
	next unless $segment =~ /^[0-9A-F]{24}$/;
	next unless $segment ge $enable_start_seg;
	unlink("$wal_dir/$segment")
	  or die "could not unlink $wal_dir/$segment: $!";
}
closedir($dh);
$node_standby->append_conf('postgresql.conf', "primary_conninfo = ''");

# The standby must not reach consistency until it has re-replayed the state
# change, so startup must not complete within the timeout.  Reaching hot
# standby below the record would make pg_ctl return success here.
my $started;
{
	local $ENV{PGCTLTIMEOUT} = 10;
	$started = $node_standby->start(fail_ok => 1);
}
is($started, 0,
	'standby withholds consistency until the state change is replayed again');

my ($ret, $out, $err) = $node_standby->psql('postgres', 'SELECT 1;');
isnt($ret, 0, 'standby refuses connections while below the state change');

# Reconnect the standby; streaming provides the removed WAL again, replay
# passes the state change and the standby becomes consistent.
$node_standby->enable_streaming($node_primary);
$node_standby->reload;
$node_standby->poll_query_until('postgres', 'SELECT true;');
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));

# The rewritten pages are again only dirty in shared buffers, so the on-disk
# pages still lack checksums.  A base backup must skip verification entirely
# and pass without mentioning checksums on stderr.  Standby backups always
# print a NOTICE about WAL archiving, so stderr is not empty.
$node_standby->command_checks_all(
	[
		'pg_basebackup', '-D',
		$node_standby->backup_dir . '/underway', '--wal-method=none',
		'--no-sync', '--checkpoint=fast'
	],
	0,
	[qr{^$}],
	[qr{^(?!.*checksum)}s],
	'backup from standby while enabling is underway succeeds');
rmtree($node_standby->backup_dir . '/underway');

# Release the enabling; its final checkpoint flushes the rewritten pages.
$node_primary->safe_psql('postgres',
	"SELECT injection_points_wakeup('datachecksums-on-before-checkpoint');");
$node_primary->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksums-on-before-checkpoint');");
wait_for_checksum_state($node_primary, 'on');
$node_primary->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_catalog.pg_stat_activity "
	  . "WHERE backend_type = 'datachecksums launcher';");

# A restartpoint on the final checkpoint lets verification resume, and a
# backup started from it must again pass.
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));
$node_standby->safe_psql('postgres', 'CHECKPOINT;');

$node_standby->command_checks_all(
	[
		'pg_basebackup', '-D',
		$node_standby->backup_dir . '/after_enable', '--wal-method=none',
		'--no-sync', '--checkpoint=fast'
	],
	0,
	[qr{^$}],
	[qr{^(?!.*checksum)}s],
	'backup from standby after enable completion succeeds');
rmtree($node_standby->backup_dir . '/after_enable');

$node_standby->stop;
$node_primary->stop;
done_testing();
