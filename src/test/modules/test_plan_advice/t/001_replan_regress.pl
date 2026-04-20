# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Run the core regression tests under pg_plan_advice to check for problems.
use strict;
use warnings FATAL => 'all';

use Cwd            qw(abs_path);
use File::Basename qw(dirname);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize the primary node
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init();

# Set up our desired configuration.
$node->append_conf('postgresql.conf', <<EOM);
shared_preload_libraries='test_plan_advice'
wal_level=replica
pg_plan_advice.always_explain_supplied_advice=false
pg_plan_advice.feedback_warnings=true
EOM
$node->start;

my $srcdir = abs_path("../../../..");

# --dlpath is needed to be able to find the location of regress.so
# and any libraries the regression tests require.
my $dlpath = dirname($ENV{REGRESS_SHLIB});

# --outputdir points to the path where to place the output files.
my $outputdir = $PostgreSQL::Test::Utils::tmp_check;

# --inputdir points to the path of the input files.
my $inputdir = "$srcdir/src/test/regress";

# Run the tests.
my $rc =
  system($ENV{PG_REGRESS} . " "
	  . "--bindir= "
	  . "--dlpath=\"$dlpath\" "
	  . "--host=" . $node->host . " "
	  . "--port=" . $node->port . " "
	  . "--schedule=$srcdir/src/test/regress/parallel_schedule "
	  . "--max-concurrent-tests=20 "
	  . "--inputdir=\"$inputdir\" "
	  . "--outputdir=\"$outputdir\"");

# Dump out the regression diffs file, if there is one
if ($rc != 0)
{
	my $diffs = "$outputdir/regression.diffs";
	if (-e $diffs)
	{
		print "=== dumping $diffs ===\n";
		print slurp_file($diffs);
		print "=== EOF ===\n";
	}
}

# Report results
is($rc, 0, 'regression tests pass');

done_testing();
