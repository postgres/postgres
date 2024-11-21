# Copyright (c) 2021-2024, PostgreSQL Global Development Group
#
# Test situation where a target data directory contains
# WAL files that were already recycled by the new primary.
#

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;
use RewindTest;

RewindTest::setup_cluster();
$node_primary->enable_archiving();
RewindTest::start_primary();

RewindTest::create_standby();
$node_standby->enable_restoring($node_primary, 0);
$node_standby->reload();

RewindTest::primary_psql("CHECKPOINT");    # last common checkpoint

# We use `perl -e "exit(1)"` as an alternative to "false", because the latter
# might not be available on Windows.
my $false = "$^X -e \"exit(1)\"";
$node_primary->append_conf(
	'postgresql.conf', qq(
archive_command = '$false'
));
$node_primary->reload();

# advance WAL on primary; this WAL segment will never make it to the archive
RewindTest::primary_psql("CREATE TABLE t(a int)");
RewindTest::primary_psql("INSERT INTO t VALUES(0)");
RewindTest::primary_psql("SELECT pg_switch_wal()");

RewindTest::promote_standby;

# new primary loses diverging WAL segment
RewindTest::standby_psql("INSERT INTO t values(0)");
RewindTest::standby_psql("SELECT pg_switch_wal()");

$node_standby->stop();
$node_primary->stop();

my ($stdout, $stderr) = run_command(
	[
		'pg_rewind', '--debug',
		'--source-pgdata', $node_standby->data_dir,
		'--target-pgdata', $node_primary->data_dir,
		'--no-sync',
	]);

like(
	$stderr,
	qr/Not removing file .* because it is required for recovery/,
	"some WAL files were skipped");

done_testing();
