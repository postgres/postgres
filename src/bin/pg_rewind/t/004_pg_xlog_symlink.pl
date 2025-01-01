
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

#
# Test pg_rewind when the target's pg_wal directory is a symlink.
#
use strict;
use warnings FATAL => 'all';
use File::Copy;
use File::Path qw(rmtree);
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;

sub run_test
{
	my $test_mode = shift;

	my $primary_xlogdir =
	  "${PostgreSQL::Test::Utils::tmp_check}/xlog_primary";

	rmtree($primary_xlogdir);
	RewindTest::setup_cluster($test_mode);

	my $test_primary_datadir = $node_primary->data_dir;

	# turn pg_wal into a symlink
	print("moving $test_primary_datadir/pg_wal to $primary_xlogdir\n");
	move("$test_primary_datadir/pg_wal", $primary_xlogdir) or die;
	dir_symlink($primary_xlogdir, "$test_primary_datadir/pg_wal") or die;

	RewindTest::start_primary();

	# Create a test table and insert a row in primary.
	primary_psql("CREATE TABLE tbl1 (d text)");
	primary_psql("INSERT INTO tbl1 VALUES ('in primary')");

	primary_psql("CHECKPOINT");

	RewindTest::create_standby($test_mode);

	# Insert additional data on primary that will be replicated to standby
	primary_psql("INSERT INTO tbl1 values ('in primary, before promotion')");

	primary_psql('CHECKPOINT');

	RewindTest::promote_standby();

	# Insert a row in the old primary. This causes the primary and standby
	# to have "diverged", it's no longer possible to just apply the
	# standby's logs over primary directory - you need to rewind.
	primary_psql("INSERT INTO tbl1 VALUES ('in primary, after promotion')");

	# Also insert a new row in the standby, which won't be present in the
	# old primary.
	standby_psql("INSERT INTO tbl1 VALUES ('in standby, after promotion')");

	RewindTest::run_pg_rewind($test_mode);

	check_query(
		'SELECT * FROM tbl1',
		qq(in primary
in primary, before promotion
in standby, after promotion
),
		'table content');

	RewindTest::clean_rewind_test();
	return;
}

# Run the test in both modes
run_test('local');
run_test('remote');

done_testing();
