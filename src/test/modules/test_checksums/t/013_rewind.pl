# Copyright (c) 2026, PostgreSQL Global Development Group

# Test pg_rewind across an online data checksum enable.
#
# A clean switchover leaves the shutdown checkpoint of the old primary
# as the last common checkpoint between the two nodes.  When data
# checksums are enabled online on the new primary before the old one is
# rewound, pg_rewind installs the control file of the new primary, which
# already claims checksums are fully enabled, while replay begins at the
# shutdown checkpoint whose record still carries the old state.  Replay
# of the WAL stretch from before the enable must run with checksums off,
# as recorded in the checkpoint, else it would verify pages which never
# had checksums written and fail recovery.
#
# The new primary requests a checkpoint right after promotion, and
# replaying its CHECKPOINT_REDO record would repair the state before
# any interesting WAL is reached.  Hold the checkpointer on the standby
# in a restartpoint over the promotion, like in the recovery test
# 041_checkpoint_at_promote, so that the post-promotion writes end up
# in WAL before the first checkpoint of the new timeline.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Old primary.  full_page_writes is off so that the updates done on the
# promoted node do not carry full page images, forcing replay on the
# rewound node to read the pages from disk.  wal_log_hints is required
# by pg_rewind on a cluster without data checksums.
my $node_a = PostgreSQL::Test::Cluster->new('node_a');
$node_a->init(allows_streaming => 1, no_data_checksums => 1);
$node_a->append_conf(
	'postgresql.conf', qq[
autovacuum = off
full_page_writes = off
wal_log_hints = on
wal_keep_size = '1GB'
log_checkpoints = on
]);
$node_a->start;

if (!$node_a->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node_a->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");
$node_a->safe_psql('postgres', "CREATE TABLE t_div (a int);");
$node_a->safe_psql('postgres', "CREATE EXTENSION injection_points;");

# Set the hint bits on t before taking the backup, so that reads on the
# promoted node do not emit full page images for its pages later.
$node_a->safe_psql('postgres', "CHECKPOINT;");
$node_a->safe_psql('postgres', "SELECT count(*) FROM t;");

$node_a->backup('backup');
my $node_b = PostgreSQL::Test::Cluster->new('node_b');
$node_b->init_from_backup($node_a, 'backup', has_streaming => 1);
$node_b->start;

$node_a->wait_for_catchup($node_b, 'replay', $node_a->lsn('insert'));
test_checksum_state($node_a, 'off');
test_checksum_state($node_b, 'off');

# Hold the next restartpoint on the standby.
$node_b->safe_psql('postgres',
	"SELECT injection_points_attach('create-restart-point', 'wait');");

# Give the restartpoint a checkpoint record to work on, then start it
# in a background session; it will block on the injection point with
# the checkpointer busy until released.
$node_a->safe_psql('postgres', "CHECKPOINT;");
$node_a->wait_for_catchup($node_b, 'replay', $node_a->lsn('insert'));

my $bg_psql = $node_b->background_psql('postgres', on_error_stop => 0);
$bg_psql->query_until(
	qr/starting_restartpoint/, q(
   \echo starting_restartpoint
   CHECKPOINT;
));
$node_b->wait_for_event('checkpointer', 'create-restart-point');

# Clean switchover: the shutdown checkpoint of A streams to B and
# becomes the last common checkpoint.
$node_a->stop('fast');

my ($stdout, $stderr) = run_command([ 'pg_controldata', $node_a->data_dir ]);
$stdout =~ /^Latest checkpoint location:\s*([0-9A-F\/]+)$/m
  or die "checkpoint location missing from pg_controldata output";
my $shutdown_ckpt = $1;
$node_b->poll_query_until('postgres',
	"SELECT pg_last_wal_replay_lsn() > '$shutdown_ckpt'::pg_lsn;")
  or die "standby never replayed the shutdown checkpoint";

my $logstart = -s $node_b->logfile;
$node_b->promote;

# Accidental restart of the old primary, diverging its timeline.
$node_a->start;
$node_a->safe_psql('postgres', "INSERT INTO t_div VALUES (1);");
$node_a->stop('fast');

# Updates on the new primary before its first checkpoint; replay of
# these on the rewound node has to read the pages from disk.
$node_b->safe_psql('postgres', "UPDATE t SET a = a WHERE a % 25 = 0;");
ok( !$node_b->log_contains("checkpoint complete", $logstart),
	"no checkpoint on the new timeline before the updates");

# Release the checkpointer; the queued post-promotion checkpoint runs
# after the updates.
$node_b->safe_psql('postgres',
	"SELECT injection_points_wakeup('create-restart-point');");
$node_b->safe_psql('postgres',
	"SELECT injection_points_detach('create-restart-point');");
$bg_psql->quit;

enable_data_checksums($node_b, wait => 'on');
test_checksum_state($node_b, 'on');

# pg_rewind refuses to run with full_page_writes disabled on the
# source; the updates it was disabled for are already in WAL.
$node_b->safe_psql('postgres', "ALTER SYSTEM SET full_page_writes = on;");
$node_b->safe_psql('postgres', "SELECT pg_reload_conf();");

command_ok(
	[
		'pg_rewind',
		'--target-pgdata' => $node_a->data_dir,
		'--source-server' => $node_b->connstr('postgres'),
	],
	'pg_rewind from the new primary');

# Replay on the rewound node must start at the shutdown checkpoint of
# the switchover, with the control file of the new primary.
my $backup_label = slurp_file($node_a->data_dir . '/backup_label');
$backup_label =~ /^CHECKPOINT LOCATION: ([0-9A-F\/]+)$/m
  or die "checkpoint location missing from backup_label";
is($1, $shutdown_ckpt, 'replay starts at the switchover checkpoint');

($stdout, $stderr) = run_command(
	[
		'pg_waldump',
		'-p' => $node_a->data_dir . '/pg_wal',
		'-t' => 1,
		'-s' => $shutdown_ckpt,
		'-n' => 1,
	]);
like($stdout, qr/CHECKPOINT_SHUTDOWN/,
	'last common checkpoint is a shutdown checkpoint');

# pg_rewind keeps the target's own checksum state in the control file it
# writes; the online enable reaches the rewound node through WAL replay
# below, not through the copied control file.
($stdout, $stderr) = run_command([ 'pg_controldata', $node_a->data_dir ]);
like(
	$stdout,
	qr/^Data page checksum version:\s*0$/m,
	'rewound node keeps its own checksum state in the control file');

# Start the rewound node as a standby of the new primary.  Replay runs
# through the pre-enable WAL stretch and the online enable.
#
# The rewind replaced the configuration files with those of the new
# primary, so put the port back.
my $connstr = $node_b->connstr;
$node_a->append_conf(
	'postgresql.conf', qq[
port = @{[$node_a->port]}
primary_conninfo = '$connstr application_name=@{[$node_a->name]}'
]);
$node_a->set_standby_mode;
$node_a->start;

$node_b->wait_for_catchup($node_a, 'replay', $node_b->lsn('insert'));
test_checksum_state($node_a, 'on');

is($node_a->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10000', 'data readable on the rewound node');
is($node_a->safe_psql('postgres', "SELECT count(*) FROM t_div;"),
	'0', 'divergent insert was rewound');

$node_a->stop('fast');
command_ok([ 'pg_checksums', '--check', '-D', $node_a->data_dir ],
	'checksums valid on the rewound node');

$node_b->stop;

done_testing();
