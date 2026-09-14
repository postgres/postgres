# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_rewind checks the data checksum states of source and target.
# Checksums enabled on the target, or at the point of divergence, with
# a source running without them are refused: the rewound server would
# verify checksums on blocks copied from a source that has none.  The
# opposite mismatch only warns; replay keeps the target's own state.
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

# Scenarios 1 and 2: checksums on from initdb.  wal_log_hints keeps the
# target eligible for pg_rewind after checksums are disabled on it.
my $node_a = PostgreSQL::Test::Cluster->new('node_a');
$node_a->init(allows_streaming => 1);
$node_a->append_conf(
	'postgresql.conf', qq[
autovacuum = off
wal_keep_size = '1GB'
wal_log_hints = on
]);
$node_a->start;
$node_a->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

$node_a->backup('backup');
my $node_b = PostgreSQL::Test::Cluster->new('node_b');
$node_b->init_from_backup($node_a, 'backup', has_streaming => 1);
$node_b->start;
$node_a->wait_for_catchup($node_b);

# Failover to B, and divergence on A.
$node_b->promote;
$node_b->safe_psql('postgres', "INSERT INTO t VALUES (0);");
$node_a->safe_psql('postgres', "INSERT INTO t VALUES (-1);");
$node_a->stop('fast');

# Offline disable on the source only: enabled target, disabled source.
$node_b->stop;
$node_b->checksum_disable_offline;
$node_b->start;
test_checksum_state($node_b, 'off');

my @rewind_cmd = (
	'pg_rewind',
	'--target-pgdata' => $node_a->data_dir,
	'--source-server' => $node_b->connstr('postgres'));

command_fails_like(
	\@rewind_cmd,
	qr/data checksums are enabled on the target server but disabled on the source server/,
	'refuses an enabled target with a disabled source');

# The refusal happens before any modification: the target still starts
# on its own timeline.
$node_a->start;
is($node_a->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001', 'target untouched by the refused rewind');
$node_a->stop('fast');

# Scenario 2: disabling the target too makes the control files match,
# but checksums were still enabled at the point of divergence, which is
# the state replay on the rewound server would resume with.
$node_a->checksum_disable_offline;
command_fails_like(
	\@rewind_cmd,
	qr/data checksums were enabled at the point of divergence but are disabled on the source server/,
	'refuses when checksums were enabled at the point of divergence');

# Scenario 3: fresh pair without checksums; offline enable on the
# source only.  Allowed with a warning, and the rewound server keeps
# checksums disabled.
my $node_c = PostgreSQL::Test::Cluster->new('node_c');
$node_c->init(allows_streaming => 1, no_data_checksums => 1);
$node_c->append_conf(
	'postgresql.conf', qq[
autovacuum = off
wal_keep_size = '1GB'
wal_log_hints = on
]);
$node_c->start;
$node_c->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

$node_c->backup('backup');
my $node_d = PostgreSQL::Test::Cluster->new('node_d');
$node_d->init_from_backup($node_c, 'backup', has_streaming => 1);
$node_d->start;
$node_c->wait_for_catchup($node_d);

$node_d->promote;
$node_d->safe_psql('postgres', "INSERT INTO t VALUES (0);");
$node_c->safe_psql('postgres', "INSERT INTO t VALUES (-1);");
$node_c->stop('fast');

$node_d->stop;
$node_d->checksum_enable_offline;
$node_d->start;
test_checksum_state($node_d, 'on');

my ($stdout, $stderr) = run_command(
	[
		'pg_rewind',
		'--target-pgdata' => $node_c->data_dir,
		'--source-server' => $node_d->connstr('postgres'),
	]);
like(
	$stderr,
	qr/data checksums are disabled on the target server but enabled on the source server/,
	'warns for a disabled target with an enabled source');
like($stderr, qr/Done!/, 'rewind completed despite the warning');

# The rewound server follows D and keeps its own state.
$node_c->append_conf('postgresql.conf', 'port = ' . $node_c->port);
$node_c->enable_streaming($node_d);
$node_c->set_standby_mode;
$node_c->start;
$node_d->wait_for_catchup($node_c);
is($node_c->safe_psql('postgres', "SELECT count(*) FROM t;"),
	'10001', 'rewound server readable as a standby');
test_checksum_state($node_c, 'off');

$node_c->stop;
$node_d->stop;
done_testing();
