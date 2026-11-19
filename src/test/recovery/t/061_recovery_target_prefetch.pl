# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that recovery ends correctly when a recovery target is reached while
# the WAL prefetcher is part-way through a later record with many block
# references.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(has_archiving => 1, allows_streaming => 1);
$node_primary->start;

$node_primary->safe_psql(
	'postgres',
	"CREATE TABLE t (i int, v text);
	 INSERT INTO t SELECT i, md5(i::text) FROM generate_series(1, 10000) i;"
);
$node_primary->backup('backup');

# An SP-GiST build WAL-logs the new index with log_newpage_range(), in
# XLOG_FPI records with up to XLR_MAX_BLOCK_ID block references each.
my $start_lsn =
  $node_primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node_primary->safe_psql('postgres',
	'CREATE INDEX t_v ON t USING spgist (v)');
my $end_lsn =
  $node_primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my $walfile = $node_primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node_primary->poll_query_until('postgres',
	"SELECT last_archived_wal >= '$walfile' FROM pg_stat_archiver")
  or die "timed out waiting for WAL to be archived";

# Stop recovery before the record preceding the first FPI record with many
# block references, rather than before the FPI record itself, because the
# prefetcher forgets about a record once it has been read (even if it is not
# replayed).  With maintenance_io_concurrency = 1, the prefetcher only looks a
# few block references ahead, so it is part-way through the FPI record when
# recovery ends.
my ($stdout, $stderr) = run_command(
	[
		'pg_waldump',
		'--path' => $node_primary->archive_dir,
		'--start' => $start_lsn,
		'--end' => $end_lsn,
	]);
$stdout =~ /prev ([0-9A-F]+\/[0-9A-F]+), desc: FPI , .*blkref #8: /
  or die "could not find an FPI record with many block references";
my $target_lsn = $1;

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup(
	$node_primary, 'backup',
	has_restoring => 1,
	standby => 0);
$node_standby->append_conf(
	'postgresql.conf', qq(
maintenance_io_concurrency = 1
recovery_target_lsn = '$target_lsn'
recovery_target_inclusive = off
recovery_target_action = 'promote'
));
$node_standby->start;
$node_standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "timed out waiting for promotion";

# Compare the LSNs as pg_lsn, so as not to depend on how they're formatted.
my ($stop_lsn) =
  slurp_file($node_standby->logfile) =~
  /recovery stopping before WAL location \(LSN\) "([0-9A-F]+\/[0-9A-F]+)"/
  or die "could not find where recovery stopped in the server log";
is( $node_standby->safe_psql(
		'postgres', "SELECT '$stop_lsn'::pg_lsn = '$target_lsn'::pg_lsn"),
	't',
	'recovery stopped at the target');
is( $node_standby->safe_psql(
		'postgres', "SELECT count(*), to_regclass('t_v') IS NULL FROM t"),
	'10000|t',
	'data matches the recovery target');

# Check that the WAL written on the new timeline can be replayed.
$node_standby->safe_psql('postgres', 'INSERT INTO t VALUES (0)');
$node_standby->stop('immediate');
$node_standby->start;
is($node_standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'10001', 'crash recovery after promotion');

done_testing();
