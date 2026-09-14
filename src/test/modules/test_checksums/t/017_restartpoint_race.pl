# Copyright (c) 2026, PostgreSQL Global Development Group

# Test a restartpoint whose flush races the replay of an online enable.
#
# Replay keeps running while CheckPointGuts() writes out the buffer pool, so
# the state at the end of the flush can be newer than the one the flush ran
# under.  Persisting it would claim checksums for pages the flush wrote out
# before the transition, against a redo pointer that predates it.

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
full_page_writes = off
));
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
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

# C1: the checkpoint the pending restartpoint will target.
$primary->safe_psql('postgres', 'CHECKPOINT;');
$primary->wait_for_catchup($standby);

# Dirty the pages of "t" without full page images and push them to disk on
# the standby while it is still "off".
$primary->safe_psql('postgres', 'UPDATE t SET a = a + 1;');
$primary->wait_for_catchup($standby);

my $relpath = $standby->safe_psql('postgres',
	"SELECT pg_relation_filepath('t'::regclass);");
my $evicted = $standby->safe_psql('postgres',
	"SELECT pg_buffercache_evict_relation('t'::regclass);");
note("evict_relation on the standby: $evicted, relpath $relpath");

# Start a restartpoint for C1 and hold it right after CheckPointGuts().
$standby->safe_psql('postgres',
	"SELECT injection_points_attach('create-restart-point','wait');");
my $bg = $standby->background_psql('postgres');
$bg->query_until(qr//, "\\echo restartpoint\nCHECKPOINT;\n");
$standby->poll_query_until('postgres',
	"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'create-restart-point';"
) or die 'timed out waiting for the restartpoint injection point';

# The startup process keeps replaying while the checkpointer is held: the
# whole online enable lands, and the rewritten pages stay dirty.
enable_data_checksums($primary, wait => 'on');
$primary->wait_for_catchup($standby);
wait_for_checksum_state($standby, 'on');

# Let the restartpoint finish; it persists the state it samples now.
$standby->safe_psql('postgres',
	"SELECT injection_points_wakeup('create-restart-point');");
$standby->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE wait_event = 'create-restart-point';"
) or die 'timed out waiting for the restartpoint to finish';
$standby->safe_psql('postgres',
	"SELECT injection_points_detach('create-restart-point');");

$standby->stop('immediate');

my ($ctl) = run_command([ 'pg_controldata', $standby->data_dir ]);
my ($after) = $ctl =~ /Data page checksum version:\s+(\d+)/;
my ($redo) = $ctl =~ /Latest checkpoint's REDO location:\s+(\S+)/;
note("standby control file after the restartpoint: $after, redo $redo");

my $page;
open(my $fh, '<', $standby->data_dir . '/' . $relpath) or die $!;
binmode $fh;
read($fh, $page, 8192);
close($fh);
my ($pd_checksum) = unpack('x8 v', $page);
note("on-disk pd_checksum of t block 0: $pd_checksum");

my $started = $standby->start(fail_ok => 1);
ok($started, 'standby restarts after a restartpoint that raced the enable');

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
