# Copyright (c) 2026, PostgreSQL Global Development Group

# Test a standby promoted right after replaying an online enable, before any
# restartpoint has flushed the rewritten pages.
#
# The end-of-recovery record persists the checksum state without flushing the
# buffer pool, while the control file still points at a restartpoint older than
# the transition.  Persisting "on" there would make a crash before the
# post-promotion checkpoint verify checksums over the pages that were flushed
# while the state was still "off"; promotion must take the full checkpoint
# instead.

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
$primary->init(allows_streaming => 1, no_data_checksums => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
full_page_writes = off
));
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION pg_buffercache;');
$primary->safe_psql('postgres',
	'CREATE TABLE t AS SELECT generate_series(1,10000) AS a;');

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq(
shared_buffers = 512MB
checkpoint_timeout = 1h
max_wal_size = 10GB
bgwriter_lru_maxpages = 0
));
$standby->start;
$primary->wait_for_catchup($standby);

test_checksum_state($primary, 'off');
test_checksum_state($standby, 'off');

# Establish the restartpoint that recovery will resume from after the crash.
$primary->safe_psql('postgres', 'CHECKPOINT;');
$primary->wait_for_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT;');

# Dirty the pages of "t" without full page images, then push them out to disk
# on the standby while checksums are still off.
$primary->safe_psql('postgres', 'UPDATE t SET a = a + 1;');
$primary->wait_for_catchup($standby);

my $relpath = $standby->safe_psql('postgres',
	"SELECT pg_relation_filepath('t'::regclass);");
my $evicted = $standby->safe_psql('postgres',
	"SELECT pg_buffercache_evict_relation('t'::regclass);");
note("evict_relation on the standby: $evicted, relpath $relpath");

# Online enable on the primary; the standby replays it into its own buffers,
# where the rewritten pages stay dirty (no restartpoint, no bgwriter).
enable_data_checksums($primary, wait => 'on');
$primary->wait_for_catchup($standby);
wait_for_checksum_state($standby, 'on');

my ($ctl) = run_command([ 'pg_controldata', $standby->data_dir ]);
my ($before) = $ctl =~ /Data page checksum version:\s+(\d+)/;
note("standby control file before the promotion: $before");

# Promote.  The end-of-recovery record persists the live state without any
# buffer flush; the checkpoint requested afterwards is not immediate.
$standby->promote;
$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery();')
  or die 'timed out waiting for the promotion';
$standby->stop('immediate');

($ctl) = run_command([ 'pg_controldata', $standby->data_dir ]);
my ($after) = $ctl =~ /Data page checksum version:\s+(\d+)/;
my ($ckpt) = $ctl =~ /Latest checkpoint location:\s+(\S+)/;
my ($redo) = $ctl =~ /Latest checkpoint's REDO location:\s+(\S+)/;
note(
	"standby control file after the promotion: $after, checkpoint $ckpt, redo $redo"
);

my $page;
open(my $fh, '<', $standby->data_dir . '/' . $relpath) or die $!;
binmode $fh;
read($fh, $page, 8192);
close($fh);
my ($pd_checksum) = unpack('x8 v', $page);
note("on-disk pd_checksum of t block 0: $pd_checksum");

my $started = $standby->start(fail_ok => 1);
ok($started,
	'promoted node restarts after crashing before its first checkpoint');

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
	is($rc, 0, 'table readable after the crash restart')
	  or diag("stderr: $stderr");
	$standby->stop('immediate');
}
else
{
	my @lines = grep { /FATAL|PANIC|invalid page|verification failed/ }
	  split(/\n/, $log);
	diag("log tail:\n" . join("\n", @lines));
	fail('table readable after the crash restart');
}

$primary->stop;

done_testing();
