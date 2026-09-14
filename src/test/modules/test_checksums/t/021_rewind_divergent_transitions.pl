# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_rewind across online data checksum transitions on both sides of a
# divergence, with the two nodes trading roles between the scenarios.
#
# Scenario 1: after a switchover the new primary enables checksums
# online, while the old primary restarts on its old timeline, advances
# its WAL beyond the enable records and runs an online enable/disable
# cycle of its own.  The old primary ends "off" with a checksum
# watermark numerically above every checksum record the new primary has
# written.  pg_rewind clamps the watermark it keeps to the divergence
# point; without the clamp, replay on the rewound node would skip the
# source's enable as already applied and stay "off" under an "on"
# primary.
#
# Scenario 2: checksums are disabled again, and after another
# switchover both nodes enable them online independently, so the
# divergence checkpoint carries "off" while both control files say
# "on".  The rewind is allowed, the target keeps its own "on" state,
# and replay re-walks the source's enable onto the already enabled
# node.
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

sub controldata_watermark
{
	my ($node) = @_;
	my ($stdout) = run_command([ 'pg_controldata', $node->data_dir ]);
	$stdout =~ /^Data checksum watermark:\s*([0-9A-F]+)\/([0-9A-F]+)$/m
	  or die "watermark missing from pg_controldata output";
	return (hex($1) << 32) + hex($2);
}

# Wait until the standby has replayed the shutdown checkpoint of the
# stopped primary, so that a subsequent promotion diverges after it and
# the shutdown checkpoint becomes the last common checkpoint.
sub wait_for_shutdown_checkpoint_replay
{
	my ($primary, $standby) = @_;
	my ($stdout) = run_command([ 'pg_controldata', $primary->data_dir ]);
	$stdout =~ /^Latest checkpoint location:\s*([0-9A-F\/]+)$/m
	  or die "checkpoint location missing from pg_controldata output";
	my $shutdown_checkpoint = $1;

	$standby->poll_query_until('postgres',
		"SELECT pg_last_wal_replay_lsn() > '$shutdown_checkpoint'::pg_lsn;")
	  or die "standby never replayed the shutdown checkpoint";
}

# Old primary, checksums off.  wal_log_hints is required by pg_rewind
# on a cluster without data checksums.
my $node_a = PostgreSQL::Test::Cluster->new('node_a');
$node_a->init(allows_streaming => 1, no_data_checksums => 1);
$node_a->append_conf(
	'postgresql.conf', qq[
autovacuum = off
wal_log_hints = on
wal_keep_size = '1GB'
]);
$node_a->start;
$node_a->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");
$node_a->safe_psql('postgres', "CREATE TABLE t_div (a int);");

$node_a->backup('backup');
my $node_b = PostgreSQL::Test::Cluster->new('node_b');
$node_b->init_from_backup($node_a, 'backup', has_streaming => 1);
$node_b->start;
$node_a->wait_for_catchup($node_b, 'replay', $node_a->lsn('insert'));

# Clean switchover to B; enable checksums online on it.
$node_a->stop('fast');
wait_for_shutdown_checkpoint_replay($node_a, $node_b);
$node_b->promote;
enable_data_checksums($node_b, wait => 'on');
test_checksum_state($node_b, 'on');

$node_b->stop('fast');
my $watermark_b = controldata_watermark($node_b);
$node_b->start;

# Accidental restart of the old primary on the old timeline.  Advance
# its WAL beyond the enable watermark of B, then run an online enable
# and disable cycle: the node ends "off" with a watermark above every
# checksum record B has written.
$node_a->start;
$node_a->safe_psql('postgres', "INSERT INTO t_div VALUES (1);");
$node_a->safe_psql('postgres',
	"CREATE TABLE t_pad AS SELECT generate_series(1,200000) AS a;");
enable_data_checksums($node_a, wait => 'on');
disable_data_checksums($node_a, wait => 'off');
test_checksum_state($node_a, 'off');
$node_a->stop('fast');

my $watermark_a = controldata_watermark($node_a);
die "test broken: target watermark not above the source's enable"
  unless $watermark_a > $watermark_b;

command_ok(
	[
		'pg_rewind',
		'--target-pgdata' => $node_a->data_dir,
		'--source-server' => $node_b->connstr('postgres'),
	],
	'pg_rewind from the promoted node');

# Start the rewound node as a standby of B.  Replay from the last
# common checkpoint runs through B's online enable, which the target
# never saw, so the rewound node must converge to "on".
my $connstr_b = $node_b->connstr;
$node_a->append_conf(
	'postgresql.conf', qq[
port = @{[$node_a->port]}
primary_conninfo = '$connstr_b application_name=@{[$node_a->name]}'
]);
$node_a->set_standby_mode;
$node_a->start;

$node_b->wait_for_catchup($node_a, 'replay', $node_b->lsn('insert'));
test_checksum_state($node_a, 'on');

is($node_a->safe_psql('postgres', "SELECT count(*) FROM t_div;"),
	'0', 'divergent insert was rewound');

# Scenario 2, reusing the pair with the roles reversed.  Disable
# checksums online so the next divergence point carries "off", and let
# A replay the change.
disable_data_checksums($node_b, wait => 'off');
$node_b->wait_for_catchup($node_a, 'replay', $node_b->lsn('insert'));
test_checksum_state($node_a, 'off');

# Clean switchover back to A; enable checksums online on it.
$node_b->stop('fast');
wait_for_shutdown_checkpoint_replay($node_b, $node_a);
$node_a->promote;
enable_data_checksums($node_a, wait => 'on');
test_checksum_state($node_a, 'on');
my $source_enable_watermark = controldata_watermark($node_a);

# The old primary restarts on its old timeline and enables checksums
# online independently: both control files say "on", the divergence
# checkpoint says "off".
$node_b->start;
$node_b->safe_psql('postgres', "INSERT INTO t_div VALUES (2);");
enable_data_checksums($node_b, wait => 'on');
test_checksum_state($node_b, 'on');
$node_b->stop('fast');

command_ok(
	[
		'pg_rewind',
		'--target-pgdata' => $node_b->data_dir,
		'--source-server' => $node_a->connstr('postgres'),
	],
	'pg_rewind with checksums enabled on both nodes');

my $connstr_a = $node_a->connstr;
$node_b->append_conf(
	'postgresql.conf', qq[
port = @{[$node_b->port]}
primary_conninfo = '$connstr_a application_name=@{[$node_b->name]}'
]);
$node_b->set_standby_mode;
$node_b->start;

$node_a->wait_for_catchup($node_b, 'replay', $node_a->lsn('insert'));
test_checksum_state($node_b, 'on');

is($node_b->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10000', 'data readable on the rewound node');
is($node_b->safe_psql('postgres', "SELECT count(*) FROM t_div;"),
	'0', 'divergent insert was rewound');

$node_b->stop('fast');
is(controldata_watermark($node_b),
	$source_enable_watermark,
	'rewound node replayed the source checksum transition');
command_ok([ 'pg_checksums', '--check', '-D', $node_b->data_dir ],
	'checksums valid on the rewound node');

$node_a->stop('fast');
command_ok(
	[ 'pg_checksums', '--check', '-D', $node_a->data_dir ],
	'checksums valid on the twice-rewound node');

done_testing();
