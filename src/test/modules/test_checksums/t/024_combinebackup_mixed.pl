# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that pg_combinebackup refuses a chain whose backups were taken under
# different data checksum states when the final state has them enabled.
#
# The output directory keeps the last backup's control file, so a full
# backup taken with checksums off plus an incremental taken after an
# offline enable would produce a directory whose control file says "on"
# while most of its blocks have no checksums.  An offline enable between
# the two backups is enough to get there: it rewrites every page but logs
# nothing, so the incremental backup does not re-ship the pages.
#
# The reverse order stays allowed: with the final backup taken with
# checksums off, stale checksums from an earlier backup are never verified.
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

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(
	no_data_checksums => 1,
	has_archiving => 1,
	allows_streaming => 1);
$node->append_conf('postgresql.conf', 'summarize_wal = on');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres',
	"CREATE TABLE t1 AS SELECT generate_series(1,200000) AS a;");
$node->safe_psql('postgres', 'CHECKPOINT;');

test_checksum_state($node, 'off');

$node->backup('full');

# Offline enable: rewrites every page, logs nothing.
$node->stop;
system_or_bail('pg_checksums', '--enable', '--pgdata', $node->data_dir);
$node->start;
test_checksum_state($node, 'on');

$node->safe_psql('postgres',
	"CREATE TABLE t2 AS SELECT generate_series(1,1000) AS a;");
$node->safe_psql('postgres', 'CHECKPOINT;');

$node->command_ok(
	[
		'pg_basebackup',
		'--pgdata' => $node->backup_dir . '/incr',
		'--dbname' => $node->connstr('postgres'),
		'--no-sync',
		'--checkpoint' => 'fast',
		'--incremental' => $node->backup_dir . '/full/backup_manifest',
	],
	'incremental backup with checksums on');

# Combining the chain must be refused: the result would say "on" while the
# blocks inherited from the full backup have no checksums.
command_fails_like(
	[
		'pg_combinebackup',
		$node->backup_dir . '/full',
		$node->backup_dir . '/incr',
		'--output' => $node->backup_dir . '/combined',
	],
	qr/only some backups have checksums enabled/,
	'pg_combinebackup refuses a chain crossing an offline enable');

# The other direction: full backup with checksums on, offline disable, then
# an incremental.  The last backup wins, so the combined cluster comes up off.
$node->backup('full2');

$node->stop;
system_or_bail('pg_checksums', '--disable', '--pgdata', $node->data_dir);
$node->start;
test_checksum_state($node, 'off');

$node->safe_psql('postgres',
	"CREATE TABLE t3 AS SELECT generate_series(1,1000) AS a;");
$node->safe_psql('postgres', 'CHECKPOINT;');

$node->command_ok(
	[
		'pg_basebackup',
		'--pgdata' => $node->backup_dir . '/incr2',
		'--dbname' => $node->connstr('postgres'),
		'--no-sync',
		'--checkpoint' => 'fast',
		'--incremental' => $node->backup_dir . '/full2/backup_manifest',
	],
	'incremental backup with checksums off');

my $restored = PostgreSQL::Test::Cluster->new('restored');
$restored->init_from_backup(
	$node, 'incr2',
	combine_with_prior => ['full2'],
	has_restoring => 1,
	standby => 0);

$restored->start;

my ($rc, $stdout, $stderr) = $restored->psql('postgres',
	"SELECT setting FROM pg_settings WHERE name = 'data_checksums';");
is($rc, 0, 'combined backup accepts connections') or diag("stderr: $stderr");
is($stdout, 'off', 'combined backup follows the last backup state');

($rc, $stdout, $stderr) =
  $restored->psql('postgres', 'SELECT count(*) FROM t3;');
is($rc, 0, 'blocks from the incremental backup are readable')
  or diag("stderr: $stderr");

($rc, $stdout, $stderr) =
  $restored->psql('postgres', 'SELECT count(*) FROM t1;');
is($rc, 0, 'blocks inherited from the full backup are readable')
  or diag("stderr: $stderr");

my $log = PostgreSQL::Test::Utils::slurp_file($restored->logfile);
unlike(
	$log,
	qr/page verification failed/,
	'no checksum verification failures in the combined backup');

$restored->stop('immediate');
$node->stop;

done_testing();
