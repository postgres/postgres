# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that pg_rewind compares the data checksum state at the point of
# divergence against the target as well as the source.
#
# A standby's checkpoints are written by its upstream primary, so a standby
# whose checksums were disabled offline still has "on" checkpoints in its
# WAL.  Rewinding it onto a promoted sibling passes every control file
# check (target off + source on only warns), yet recovery after the rewind
# adopts the divergence checkpoint's state and the rewound server would
# verify checksums over the pages it wrote while it was locally "off".
# pg_rewind must refuse; after an offline enable on the target, which
# rewrites all of its pages, the rewind goes through.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# This test suite is expensive to execute, require PG_TEST_EXTRA to contain
# 'checksum_extended' to run it.
if ($ENV{PG_TEST_EXTRA})
{
	plan skip_all => 'Expensive data checksums test disabled'
	  unless ($ENV{PG_TEST_EXTRA} =~ /\bchecksum_extended\b/);
}
else
{
	plan skip_all => 'Expensive data checksums test disabled';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq[
autovacuum = off
wal_keep_size = '1GB'
wal_log_hints = on
]);
$primary->start;
$primary->safe_psql('postgres',
	"CREATE TABLE t0 AS SELECT generate_series(1,10000) AS a;");

$primary->backup('backup');

my $lagging = PostgreSQL::Test::Cluster->new('lagging');
$lagging->init_from_backup($primary, 'backup', has_streaming => 1);
$lagging->start;

my $failover = PostgreSQL::Test::Cluster->new('failover');
$failover->init_from_backup($primary, 'backup', has_streaming => 1);
$failover->start;

$primary->wait_for_catchup($lagging);
$primary->wait_for_catchup($failover);

test_checksum_state($primary, 'on');
test_checksum_state($lagging, 'on');
test_checksum_state($failover, 'on');

# Offline-disable checksums on the lagging standby only.  This is a
# divergence 012_offline_standby.pl declares survivable: the standby keeps
# its own state, warns, and stays readable.
$lagging->stop;
system_or_bail('pg_checksums', '--disable', '--pgdata', $lagging->data_dir);
$lagging->start;
test_checksum_state($lagging, 'off');

# Give the lagging standby plenty of pages to write while it is locally
# "off", and force a restartpoint so they reach disk without checksums.
# The other standby replays the same WAL with checksums on.
$primary->safe_psql('postgres',
	"CREATE TABLE t1 AS SELECT generate_series(1,200000) AS a;");
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->wait_for_catchup($lagging);
$primary->wait_for_catchup($failover);
$lagging->safe_psql('postgres', "CHECKPOINT;");

# Take the "failover" standby out of the picture.  It is promoted from
# this position later, so everything replayed from here on is the part of
# the WAL that diverges and that pg_rewind will roll back.  t1 is already
# replayed everywhere, so its blocks are *not* rolled back.
$failover->stop;

$primary->safe_psql('postgres',
	"CREATE TABLE t2 AS SELECT generate_series(1,1000) AS a;");
$primary->wait_for_catchup($lagging);

# Failover: promote the other standby, which forks the timeline behind the
# replay position of the lagging standby.
$primary->stop('fast');
$failover->start;
$failover->promote;
$failover->safe_psql('postgres',
	"CREATE TABLE t3 AS SELECT generate_series(1,1000) AS a;");
$failover->safe_psql('postgres', "CHECKPOINT;");

# Re-attaching the lagging standby with pg_rewind must be refused: the
# divergence checkpoint says "on" while the target is "off", so the rewound
# server would verify checksums over the blocks it keeps.
$lagging->stop;

command_fails_like(
	[
		'pg_rewind',
		'--target-pgdata' => $lagging->data_dir,
		'--source-server' => $failover->connstr('postgres'),
	],
	qr/data checksums were enabled at the point of divergence but are disabled on the target server/,
	'pg_rewind refuses a target whose divergence checkpoint has checksums');

# Enabling checksums offline on the target rewrites all of its pages, after
# which the rewind is safe.
system_or_bail('pg_checksums', '--enable', '--pgdata', $lagging->data_dir);

my ($stdout, $stderr) = run_command(
	[
		'pg_rewind',
		'--target-pgdata' => $lagging->data_dir,
		'--source-server' => $failover->connstr('postgres'),
	]);
like($stderr, qr/Done!/, 'pg_rewind completes after the offline enable');

$lagging->append_conf('postgresql.conf', 'port = ' . $lagging->port);
$lagging->enable_streaming($failover);
$lagging->set_standby_mode;
$lagging->start;

my ($rc, $out, $err) =
  $lagging->psql('postgres',
	"SELECT setting FROM pg_settings " . "WHERE name = 'data_checksums';");
is($rc, 0, 'rewound standby accepts connections') or diag("stderr: $err");
is($out, 'on', 'rewound standby resumes with checksums on');

($rc, $out, $err) = $lagging->psql('postgres', "SELECT count(*) FROM t1;");
is($rc, 0, 'blocks kept from the target stay readable')
  or diag("stderr: $err");

my $log = PostgreSQL::Test::Utils::slurp_file($lagging->logfile);
unlike(
	$log,
	qr/page verification failed/,
	'no checksum verification failures on the rewound standby');

$lagging->stop('immediate');
$failover->stop;
done_testing();
