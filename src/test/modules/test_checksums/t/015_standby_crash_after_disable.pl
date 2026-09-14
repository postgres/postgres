# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a standby crashing after a replayed online disable, but before
# its next restartpoint, restarts cleanly.
#
# Pages dirtied before the XLOG2_CHECKSUMS record and evicted after it are
# written out under the new "off" state, without checksums.  The control
# file must follow the record immediately in this direction: a crash-restart
# initializes checksum verification from the control file, and one still
# saying "on" would fail verification on exactly those pages while replaying
# records older than the state change.

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

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
));
$primary->start;

test_checksum_state($primary, 'on');

$primary->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,1000) AS a;");

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);

# Small buffer pool so that replaying a bulk load evicts the pages we care
# about, and no restartpoints so the control file stays where it is.
$standby->append_conf(
	'postgresql.conf', qq(
shared_buffers = 1MB
checkpoint_timeout = 1h
max_wal_size = 10GB
bgwriter_delay = 10000
));
$standby->start;
$primary->wait_for_catchup($standby);

test_checksum_state($standby, 'on');

# No full page images, so that replay has to read the pages of "t" back from
# disk instead of overwriting them from the WAL.
$primary->append_conf('postgresql.conf', 'full_page_writes = off');
$primary->reload;
$primary->safe_psql('postgres', 'CHECKPOINT;');

# Establish the restartpoint that recovery will resume from after the crash.
$standby->safe_psql('postgres', 'CHECKPOINT;');
$primary->wait_for_catchup($standby);

# Dirty the pages of "t" on the standby, *before* the state change.  They are
# not flushed: the standby has no restartpoint from here on.
$primary->safe_psql('postgres', 'UPDATE t SET a = a + 1;');
$primary->wait_for_catchup($standby);

# Online disable.  The standby replays it and switches to "off".
$primary->safe_psql('postgres', 'SELECT pg_disable_data_checksums();');
$primary->wait_for_catchup($standby);
wait_for_checksum_state($standby, 'off');

my ($ctl_before) = run_command([ 'pg_controldata', $standby->data_dir ]);
my ($ctl_state) = $ctl_before =~ /Data page checksum version:\s+(\d+)/;
note(   "standby control file data_checksum_version after the disable: "
	  . "$ctl_state (0 = off, 1 = on)");

# Force the standby to evict the dirty pages of "t" now that it is "off":
# they are written back without checksums.
$primary->safe_psql('postgres',
	"CREATE TABLE filler AS SELECT generate_series(1,300000) AS a;");
$primary->wait_for_catchup($standby);

# Crash the standby before it gets a chance to run a restartpoint.
$standby->stop('immediate');

my $started = $standby->start(fail_ok => 1);
ok($started,
		'standby restarts after crashing between a checksum state '
	  . 'change and the next restartpoint');

my $log = PostgreSQL::Test::Utils::slurp_file($standby->logfile);
unlike(
	$log,
	qr/page verification failed/,
	'no checksum verification failures while replaying');
unlike($log, qr/invalid page in block/, 'no invalid pages while replaying');

if ($started)
{
	my ($rc, $stdout, $stderr) =
	  $standby->psql('postgres', 'SELECT count(*) FROM t;');
	is($rc, 0, 'the pages written while "off" are readable')
	  or diag("stderr: $stderr");
	$standby->stop('immediate');
}
else
{
	my @lines = grep { /FATAL|PANIC|invalid page|verification failed/ }
	  split(/\n/, $log);
	diag("standby log tail:\n" . join("\n", @lines[ -12 .. -1 ]))
	  if @lines >= 12;
	fail('the pages written while "off" are readable');
}

$primary->stop;

done_testing();
