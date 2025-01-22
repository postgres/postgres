
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Run the standard regression tests with streaming replication
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Basename;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

# Increase some settings that Cluster->new makes too low by default.
$node_primary->adjust_conf('postgresql.conf', 'max_connections', '25');
$node_primary->append_conf('postgresql.conf',
	'max_prepared_transactions = 10');

# Enable pg_stat_statements to force tests to do query jumbling.
# pg_stat_statements.max should be large enough to hold all the entries
# of the regression database.
$node_primary->append_conf(
	'postgresql.conf',
	qq{shared_preload_libraries = 'pg_stat_statements'
pg_stat_statements.max = 50000
compute_query_id = 'regress'
});

# We'll stick with Cluster->new's small default shared_buffers, but since that
# makes synchronized seqscans more probable, it risks changing the results of
# some test queries.  Disable synchronized seqscans to prevent that.
$node_primary->append_conf('postgresql.conf', 'synchronize_seqscans = off');

# WAL consistency checking is resource intensive so require opt-in with the
# PG_TEST_EXTRA environment variable.
if (   $ENV{PG_TEST_EXTRA}
	&& $ENV{PG_TEST_EXTRA} =~ m/\bwal_consistency_checking\b/)
{
	$node_primary->append_conf('postgresql.conf',
		'wal_consistency_checking = all');
}

$node_primary->start;
is( $node_primary->psql(
		'postgres',
		qq[SELECT pg_create_physical_replication_slot('standby_1');]),
	0,
	'physical slot created on primary');
my $backup_name = 'my_backup';

# Take backup
$node_primary->backup($backup_name);

# Create streaming standby linking to primary
my $node_standby_1 = PostgreSQL::Test::Cluster->new('standby_1');
$node_standby_1->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby_1->append_conf('postgresql.conf',
	"primary_slot_name = standby_1");
$node_standby_1->append_conf('postgresql.conf',
	'max_standby_streaming_delay = 600s');
$node_standby_1->start;

my $dlpath = dirname($ENV{REGRESS_SHLIB});
my $outputdir = $PostgreSQL::Test::Utils::tmp_check;

# Run the regression tests against the primary.
my $extra_opts = $ENV{EXTRA_REGRESS_OPTS} || "";
my $rc =
  system($ENV{PG_REGRESS}
	  . " $extra_opts "
	  . "--dlpath=\"$dlpath\" "
	  . "--bindir= "
	  . "--host="
	  . $node_primary->host . " "
	  . "--port="
	  . $node_primary->port . " "
	  . "--schedule=../regress/parallel_schedule "
	  . "--max-concurrent-tests=20 "
	  . "--inputdir=../regress "
	  . "--outputdir=\"$outputdir\"");
if ($rc != 0)
{
	# Dump out the regression diffs file, if there is one
	my $diffs = "$outputdir/regression.diffs";
	if (-e $diffs)
	{
		print "=== dumping $diffs ===\n";
		print slurp_file($diffs);
		print "=== EOF ===\n";
	}
}
is($rc, 0, 'regression tests pass');

# Clobber all sequences with their next value, so that we don't have
# differences between nodes due to caching.
$node_primary->psql('regression',
	"select setval(seqrelid, nextval(seqrelid)) from pg_sequence");

# Wait for standby to catch up
$node_primary->wait_for_replay_catchup($node_standby_1);

# Perform a logical dump of primary and standby, and check that they match
command_ok(
	[
		'pg_dumpall',
		'--file' => $outputdir . '/primary.dump',
		'--no-sync',
		'--port' => $node_primary->port,
		'--no-unlogged-table-data',    # if unlogged, standby has schema only
	],
	'dump primary server');
command_ok(
	[
		'pg_dumpall',
		'--file' => $outputdir . '/standby.dump',
		'--no-sync',
		'--port' => $node_standby_1->port,
	],
	'dump standby server');
command_ok(
	[ 'diff', $outputdir . '/primary.dump', $outputdir . '/standby.dump', ],
	'compare primary and standby dumps');

# Likewise for the catalogs of the regression database, after disabling
# autovacuum to make fields like relpages stop changing.
$node_primary->append_conf('postgresql.conf', 'autovacuum = off');
$node_primary->restart;
$node_primary->wait_for_replay_catchup($node_standby_1);
command_ok(
	[
		'pg_dump',
		'--schema' => 'pg_catalog',
		'--file' => $outputdir . '/catalogs_primary.dump',
		'--no-sync',
		'--port', $node_primary->port,
		'--no-unlogged-table-data',
		'regression',
	],
	'dump catalogs of primary server');
command_ok(
	[
		'pg_dump',
		'--schema' => 'pg_catalog',
		'--file' => $outputdir . '/catalogs_standby.dump',
		'--no-sync',
		'--port' => $node_standby_1->port,
		'regression',
	],
	'dump catalogs of standby server');
command_ok(
	[
		'diff',
		$outputdir . '/catalogs_primary.dump',
		$outputdir . '/catalogs_standby.dump',
	],
	'compare primary and standby catalog dumps');

# Check some data from pg_stat_statements.
$node_primary->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statements');
# This gathers data based on the first characters for some common query types,
# checking that reports are generated for SELECT, DMLs, and DDL queries with
# CREATE.
my $result = $node_primary->safe_psql(
	'postgres',
	qq{WITH select_stats AS
  (SELECT upper(substr(query, 1, 6)) AS select_query
     FROM pg_stat_statements
     WHERE upper(substr(query, 1, 6)) IN ('SELECT', 'UPDATE',
                                          'INSERT', 'DELETE',
                                          'CREATE'))
  SELECT select_query, count(select_query) > 1 AS some_rows
    FROM select_stats
    GROUP BY select_query ORDER BY select_query;});
is( $result, qq(CREATE|t
DELETE|t
INSERT|t
SELECT|t
UPDATE|t), 'check contents of pg_stat_statements on regression database');

$node_standby_1->stop;
$node_primary->stop;

done_testing();
