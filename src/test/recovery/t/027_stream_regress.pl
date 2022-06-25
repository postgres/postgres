# Run the standard regression tests with streaming replication
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Basename;

if (PostgreSQL::Test::Utils::has_wal_read_bug)
{
	# We'd prefer to use Test::More->builder->todo_start, but the bug causes
	# this test file to die(), not merely to fail.
	plan skip_all => 'filesystem bug';
}

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

# Increase some settings that Cluster->new makes too low by default.
$node_primary->adjust_conf('postgresql.conf', 'max_connections', '25');
$node_primary->append_conf('postgresql.conf',
	'max_prepared_transactions = 10');
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

my $dlpath    = dirname($ENV{REGRESS_SHLIB});
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
$node_primary->wait_for_catchup($node_standby_1, 'replay',
	$node_primary->lsn('insert'));

# Perform a logical dump of primary and standby, and check that they match
command_ok(
	[
		'pg_dumpall', '-f', $outputdir . '/primary.dump',
		'--no-sync',  '-p', $node_primary->port,
		'--no-unlogged-table-data'    # if unlogged, standby has schema only
	],
	'dump primary server');
command_ok(
	[
		'pg_dumpall', '-f', $outputdir . '/standby.dump',
		'--no-sync', '-p', $node_standby_1->port
	],
	'dump standby server');
command_ok(
	[ 'diff', $outputdir . '/primary.dump', $outputdir . '/standby.dump' ],
	'compare primary and standby dumps');

$node_standby_1->stop;
$node_primary->stop;

done_testing();
