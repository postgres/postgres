# Copyright (c) 2026, PostgreSQL Global Development Group

# Offline checksum changes with pg_checksums are local to one node.  A
# standby must neither adopt the state of the primary from replayed
# checkpoint records, nor lose its own offline change to them.
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
	'postgresql.conf', qq[
autovacuum = off
wal_keep_size = '1GB'
]);
$primary->start;
$primary->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby);

test_checksum_state($primary, 'off');
test_checksum_state($standby, 'off');

# Scenario 1: enable offline on the primary only.  The standby must
# stay off, warn about the mismatch, and remain readable.
$standby->stop;
$primary->stop;
$primary->checksum_enable_offline;
$primary->start;
$standby->start;

test_checksum_state($primary, 'on');
test_checksum_state($standby, 'off');

my $logstart = -s $standby->logfile;
$primary->safe_psql('postgres', "INSERT INTO t VALUES (0);");
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->wait_for_catchup($standby);

test_checksum_state($standby, 'off');
is($standby->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001', 'standby readable after offline enable on the primary');

$standby->wait_for_log(qr/does not match the state "on" in the replayed WAL/,
	$logstart);

$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->wait_for_catchup($standby);
my $log = PostgreSQL::Test::Utils::slurp_file($standby->logfile, $logstart);
my @warnings = $log =~ /(does not match the state)/g;
is(scalar(@warnings), 1, 'mismatch warned once per remote value');

# Matching states re-arm the warning: undo the divergence on the primary,
# then diverge again to the same value, all without restarting the standby.
$primary->stop;
$primary->checksum_disable_offline;
$logstart = -s $standby->logfile;
$primary->start;
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->wait_for_catchup($standby);
$log = PostgreSQL::Test::Utils::slurp_file($standby->logfile, $logstart);
unlike(
	$log,
	qr/does not match the state/,
	'no warning while the states match again');
like(
	$log,
	qr/now agrees with the replayed WAL/,
	'convergence is reported once the states match again');

$primary->stop;
$primary->checksum_enable_offline;
$primary->start;
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->wait_for_catchup($standby);
$standby->wait_for_log(qr/does not match the state "on" in the replayed WAL/,
	$logstart);
test_checksum_state($standby, 'off');

# The local state survives both clean and immediate restarts.
$standby->restart;
test_checksum_state($standby, 'off');
$standby->stop('immediate');
$standby->start;
test_checksum_state($standby, 'off');

# Still in scenario 1's divergence: take a base backup *from the
# standby*.  A backup taken on a standby uses the last restartpoint as
# its starting checkpoint (do_pg_backup_start()), so the record at the
# redo point was written by the upstream primary and carries the
# primary's state.  Recovery from such a backup must not adopt that
# state: the files were copied from the standby, and the new node
# would come up verifying checksums its files do not have.

# Make sure the standby has written pages under its own "off" state, so
# its files really do lack checksums.
$primary->safe_psql('postgres',
	"CREATE TABLE t2 AS SELECT generate_series(1,50000) AS a;");
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->wait_for_catchup($standby);
$standby->safe_psql('postgres', "SELECT count(*) FROM t2;");

$standby->backup('from_standby');
my $newnode = PostgreSQL::Test::Cluster->new('newnode');
$newnode->init_from_backup($standby, 'from_standby');

# Stream from the primary, not from the standby the files were copied
# from, so the new node replays the "on" primary's records.
$newnode->enable_streaming($primary);
$newnode->start;

# The new node's files all came from a cluster running with checksums
# off.  Anything but "off" here means it adopted the primary's state
# through the checkpoint record at the redo point.
my ($rc, $stdout, $stderr) = $newnode->psql('postgres',
	"SELECT setting FROM pg_settings WHERE name = 'data_checksums';");
is($rc, 0, 'the node copied from the standby accepts connections')
  or diag("stderr: $stderr");
is($stdout, 'off', 'backup of an "off" standby comes up with checksums off');

# A checkpoint record streamed from the "on" primary must not flip the
# state either.
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->wait_for_catchup($newnode);
test_checksum_state($newnode, 'off');

# And it must be able to read the pages the standby wrote without
# checksums.
(undef, $stdout, $stderr) =
  $newnode->psql('postgres', "SELECT count(*) FROM t2;");
is($stdout, '50000', 'pages copied from the standby are readable')
  or diag("stderr: $stderr");

my $newnode_log = PostgreSQL::Test::Utils::slurp_file($newnode->logfile);
unlike(
	$newnode_log,
	qr/page verification failed/,
	'no checksum verification failures on the node copied from the standby');

$newnode->stop('immediate');

# Converge the cluster: enable offline on the standby too.
$standby->stop;
command_checks_all(
	[ 'pg_checksums', '--enable', '-D', $standby->data_dir ],
	0, [qr/appears to be a standby/],
	[], 'standby-role notice on offline enable');
$standby->start;
test_checksum_state($standby, 'on');
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001', 'standby readable after converging');

# Scenario 2: disable offline on the standby only.  The replayed
# checkpoint records of the still-enabled primary must not override it.
$standby->stop;
$standby->checksum_disable_offline;
$standby->start;
test_checksum_state($standby, 'off');
test_checksum_state($primary, 'on');

$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->wait_for_catchup($standby);
test_checksum_state($standby, 'off');

# Restartpoints must persist the local state, not the replayed copy.
$standby->safe_psql('postgres', "CHECKPOINT;");
$standby->restart;
test_checksum_state($standby, 'off');
$standby->stop('immediate');
$standby->start;
test_checksum_state($standby, 'off');

is($standby->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001', 'standby readable with checksums disabled locally');

# Scenario 3: crash-restart right after an online transition, before the
# next restartpoint.  Replay then resumes from an older restartpoint whose
# checkpoint records still carry the pre-transition state.  Those must
# still match the state seeded from the control file, and the transition
# itself must be re-established by re-replaying the XLOG2_CHECKSUMS record,
# without a spurious mismatch warning along the way.

# Converge first: bring the standby back to "on" offline.
$standby->stop;
$standby->checksum_enable_offline;
$standby->start;
test_checksum_state($standby, 'on');
test_checksum_state($primary, 'on');

disable_data_checksums($primary, wait => 'off');
$primary->wait_for_catchup($standby);
wait_for_checksum_state($standby, 'off');

# Crash-restart the standby immediately, before any restartpoint has had a
# chance to persist the new state to its control file.
$logstart = -s $standby->logfile;
$standby->stop('immediate');
$standby->start;
$primary->wait_for_catchup($standby);

test_checksum_state($standby, 'off');
is($standby->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001',
	'standby readable after crash-restart across an online transition');

$log = PostgreSQL::Test::Utils::slurp_file($standby->logfile, $logstart);
unlike(
	$log,
	qr/does not match the state/,
	'no spurious mismatch warning after crash-restart across an online transition'
);

# Scenario 4: a standby stopped while replaying an interrupted online
# transition keeps the interrupted state in its own control file, and
# pg_checksums must refuse to touch it.  A primary is never caught this
# way, as its checksums launcher resolves inprogress-on back to off from
# its exit cleanup.  A standby has no launcher; it carries forward
# whatever the last replayed record left it in.

# Block an online enable on the primary at inprogress-on with a
# blocking temp table, same trick as in 004_offline.pl.
my $bsession = $primary->background_psql('postgres');
$bsession->query_safe('CREATE TEMPORARY TABLE tt (a integer);');
enable_data_checksums($primary, wait => 'inprogress-on');

# The standby picks up the in-progress state from the XLOG2_CHECKSUMS record.
wait_for_checksum_state($standby, 'inprogress-on');

# Stop the standby cleanly; its restartpoint persists inprogress-on to
# its own control file, since nothing on a standby resolves it away.
$standby->stop;

command_fails_like(
	[ 'pg_checksums', '--enable', '-D', $standby->data_dir ],
	qr/online data checksum state transition was interrupted/,
	'pg_checksums --enable refuses a standby stopped mid-transition');
command_fails_like(
	[ 'pg_checksums', '--check', '-D', $standby->data_dir ],
	qr/online data checksum state transition was interrupted/,
	'pg_checksums --check refuses a standby stopped mid-transition');
command_fails_like(
	[ 'pg_checksums', '--disable', '-D', $standby->data_dir ],
	qr/online data checksum state transition was interrupted/,
	'pg_checksums --disable refuses a standby stopped mid-transition');

$standby->start;
wait_for_checksum_state($standby, 'inprogress-on');

# The primary still sits at inprogress-on, so a base backup taken now
# copies a control file and a redo point that both carry the
# in-progress state; replay of the XLOG2_CHECKSUMS records completes
# the transition on the new standby.
$primary->backup('inprogress_backup');
my $standby2 = PostgreSQL::Test::Cluster->new('standby2');
$standby2->init_from_backup($primary, 'inprogress_backup',
	has_streaming => 1);
$standby2->start;

# Backup label recovery adopts the redo point's state immediately,
# before any further WAL is replayed.
wait_for_checksum_state($standby2, 'inprogress-on');

$bsession->quit;
wait_for_checksum_state($primary, 'on');
$primary->wait_for_catchup($standby);
wait_for_checksum_state($standby, 'on');

is($standby->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001', 'standby readable once the transition completes');

# Scenario 4 continued: the backup taken mid-transition must also
# complete the transition and stay readable.
$primary->wait_for_catchup($standby2);
wait_for_checksum_state($standby2, 'on');

is($standby2->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001', 'mid-transition backup standby readable after the transition');

$standby2->stop;

# Nothing along this standby's replay can legitimately disagree: it
# started out at the same inprogress-on state as the primary.
my $standby2_log = PostgreSQL::Test::Utils::slurp_file($standby2->logfile);
unlike(
	$standby2_log,
	qr/does not match the state/,
	'no mismatch warning on a backup taken mid-transition');

# No extra CHECKPOINT needed: the transition's completion checkpoint
# wrote every page with a checksum, and the shutdown restartpoint
# flushes the rest.
command_ok(
	[ 'pg_checksums', '--check', '-D', $standby2->data_dir ],
	'checksums valid on the mid-transition backup standby');

$standby->stop;
$primary->stop;
done_testing();
