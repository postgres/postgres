#
# Test pg_rewind when the target's pg_xlog directory is a symlink.
#
use strict;
use warnings;
use File::Copy;
use File::Path qw(rmtree);
use TestLib;
use Test::More;
if ($windows_os)
{
	plan skip_all => 'symlinks not supported on Windows';
	exit;
}
else
{
	plan tests => 4;
}

use RewindTest;

sub run_test
{
	my $test_mode = shift;

	my $master_xlogdir = "${TestLib::tmp_check}/xlog_master";

	rmtree($master_xlogdir);
	RewindTest::setup_cluster();

	my $test_master_datadir = $node_master->data_dir;

	# turn pg_xlog into a symlink
	print("moving $test_master_datadir/pg_xlog to $master_xlogdir\n");
	move("$test_master_datadir/pg_xlog", $master_xlogdir) or die;
	symlink($master_xlogdir, "$test_master_datadir/pg_xlog") or die;

	RewindTest::start_master();

	# Create a test table and insert a row in master.
	master_psql("CREATE TABLE tbl1 (d text)");
	master_psql("INSERT INTO tbl1 VALUES ('in master')");

	master_psql("CHECKPOINT");

	RewindTest::create_standby();

	# Insert additional data on master that will be replicated to standby
	master_psql("INSERT INTO tbl1 values ('in master, before promotion')");

	master_psql('CHECKPOINT');

	RewindTest::promote_standby();

	# Insert a row in the old master. This causes the master and standby
	# to have "diverged", it's no longer possible to just apply the
	# standy's logs over master directory - you need to rewind.
	master_psql("INSERT INTO tbl1 VALUES ('in master, after promotion')");

	# Also insert a new row in the standby, which won't be present in the
	# old master.
	standby_psql("INSERT INTO tbl1 VALUES ('in standby, after promotion')");

	RewindTest::run_pg_rewind($test_mode);

	check_query(
		'SELECT * FROM tbl1',
		qq(in master
in master, before promotion
in standby, after promotion
),
		'table content');

	RewindTest::clean_rewind_test();
}

# Run the test in both modes
run_test('local');
run_test('remote');

exit(0);
