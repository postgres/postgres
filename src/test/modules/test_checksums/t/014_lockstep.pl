# Copyright (c) 2026, PostgreSQL Global Development Group

# The lockstep procedure for offline checksum changes in a replication
# setup: stop all nodes, run pg_checksums on all of them, restart.
# Replay of checkpoint records written before the change must not
# revert the state of the standby.
#
# The same pair then runs the procedure in the disable direction, and
# finally across a divergence: after a failover both nodes get their
# checksums enabled offline, while the last common checkpoint still
# carries "off".  pg_rewind must keep the offline "on" on the rewound
# node, since no record in the replayed WAL could ever restore it.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# wal_log_hints keeps the pair eligible for pg_rewind without checksums.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1, no_data_checksums => 1);
$primary->append_conf(
	'postgresql.conf', qq[
autovacuum = off
wal_keep_size = '1GB'
wal_log_hints = on
]);
$primary->start;
$primary->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby);

# Part 1: the lockstep procedure in the enable direction.  Stop the
# standby first: the WAL written after this point is replayed only
# after the offline switch, and every checkpoint record in it still
# carries the old state.
$standby->stop;
$primary->safe_psql('postgres', "UPDATE t SET a = a WHERE a % 10 = 0;");
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->safe_psql('postgres', "UPDATE t SET a = a WHERE a % 10 = 1;");
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->stop;

# The lockstep procedure.
$primary->checksum_enable_offline;
$standby->checksum_enable_offline;
$primary->start;
$standby->start;

# The standby replays the pre-switch checkpoints; its state must not
# revert to off.
$primary->wait_for_catchup($standby);
$standby->wait_for_log(qr/does not match the state "off" in the replayed WAL/,
	0);
test_checksum_state($standby, 'on');
test_checksum_state($primary, 'on');

is($standby->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10000', 'standby readable after lockstep enable');

# Crash the standby and replay the same stretch again.
$standby->stop('immediate');
$standby->start;
$primary->wait_for_catchup($standby);
test_checksum_state($standby, 'on');
is($standby->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10000', 'standby readable after crash restart');

# Once a post-switch checkpoint has been replayed the states match and
# no warning may be logged.
my $logstart = -s $standby->logfile;
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->wait_for_catchup($standby);
my $log = PostgreSQL::Test::Utils::slurp_file($standby->logfile, $logstart);
unlike(
	$log,
	qr/does not match the state/,
	'no mismatch warning once the states match');

# Every page the standby wrote in this window must carry a checksum.
$standby->safe_psql('postgres', 'CHECKPOINT;');
$standby->stop;
command_ok([ 'pg_checksums', '--check', '-D', $standby->data_dir ],
	'checksums valid on the standby');

# Part 2: the lockstep procedure in the other direction.  The standby
# is already stopped; give it a pre-switch WAL stretch to replay, with
# every checkpoint record in it still carrying "on" -- an explicit
# checkpoint plus the shutdown checkpoint written by stop().
$primary->safe_psql('postgres', "UPDATE t SET a = a WHERE a % 10 = 2;");
$primary->safe_psql('postgres', "CHECKPOINT;");
$primary->stop;

$primary->checksum_disable_offline;
$standby->checksum_disable_offline;

my $disable_logstart = -s $standby->logfile;
$primary->start;
$standby->start;

# The standby replays the pre-switch checkpoints; its state must not
# revert to on.
$primary->wait_for_catchup($standby);
$standby->wait_for_log(qr/does not match the state "on" in the replayed WAL/,
	$disable_logstart);
test_checksum_state($standby, 'off');
test_checksum_state($primary, 'off');

is($standby->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10000', 'standby readable after lockstep disable');

# Part 3: failover, divergence and pg_rewind across offline enables.
# The roles swap from here on: the standby becomes the new primary and
# the old primary is rewound to follow it, but the variables keep the
# names they had above.
#
# Failover: promote the standby, then diverge the old primary.
$standby->promote;
$standby->safe_psql('postgres', "INSERT INTO t VALUES (0);");
$primary->safe_psql('postgres', "INSERT INTO t VALUES (-1);");
$primary->stop;

# The lockstep procedure across the divergence: both nodes get their
# checksums enabled offline.
$primary->checksum_enable_offline;
$standby->stop;
$standby->checksum_enable_offline;
$standby->start;
test_checksum_state($standby, 'on');

# The states match, so the rewind proceeds without complaint.
command_ok(
	[
		'pg_rewind',
		'--target-pgdata' => $primary->data_dir,
		'--source-server' => $standby->connstr('postgres'),
	],
	'pg_rewind with checksums enabled offline on both nodes');

# The rewind replaced the configuration files with those of the source,
# so put the port back.  The copied file also carries the source's own
# stale primary_conninfo; enable_streaming() below appends ours, which
# wins as the later entry.
$primary->append_conf(
	'postgresql.conf', qq[
port = @{[$primary->port]}
]);
$primary->enable_streaming($standby);

my $rewind_logstart = -s $primary->logfile;
$primary->start;
$standby->wait_for_catchup($primary);

is($primary->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001', 'rewound server readable as a standby');

# The offline enable survives: replay from the common checkpoint must
# not resurrect the pre-divergence "off".
test_checksum_state($primary, 'on');

$log =
  PostgreSQL::Test::Utils::slurp_file($primary->logfile, $rewind_logstart);
unlike(
	$log,
	qr/does not match the state/,
	'no divergence reported between the rewound server and its source');

$primary->stop;
$standby->stop;

done_testing();
