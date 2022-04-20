
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test how pg_rewind reacts to extra files and directories in the data dirs.

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use Test::More;

use File::Find;

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;


sub run_test
{
	my $test_mode = shift;

	RewindTest::setup_cluster($test_mode);
	RewindTest::start_primary();

	my $test_primary_datadir = $node_primary->data_dir;

	# Create a subdir and files that will be present in both
	mkdir "$test_primary_datadir/tst_both_dir";
	append_to_file "$test_primary_datadir/tst_both_dir/both_file1",
	  "in both1";
	append_to_file "$test_primary_datadir/tst_both_dir/both_file2",
	  "in both2";
	mkdir "$test_primary_datadir/tst_both_dir/both_subdir/";
	append_to_file
	  "$test_primary_datadir/tst_both_dir/both_subdir/both_file3",
	  "in both3";

	RewindTest::create_standby($test_mode);

	# Create different subdirs and files in primary and standby
	my $test_standby_datadir = $node_standby->data_dir;

	mkdir "$test_standby_datadir/tst_standby_dir";
	append_to_file "$test_standby_datadir/tst_standby_dir/standby_file1",
	  "in standby1";
	append_to_file "$test_standby_datadir/tst_standby_dir/standby_file2",
	  "in standby2";
	append_to_file
	  "$test_standby_datadir/tst_standby_dir/standby_file3 with 'quotes'",
	  "in standby3";
	mkdir "$test_standby_datadir/tst_standby_dir/standby_subdir/";
	append_to_file
	  "$test_standby_datadir/tst_standby_dir/standby_subdir/standby_file4",
	  "in standby4";

	mkdir "$test_primary_datadir/tst_primary_dir";
	append_to_file "$test_primary_datadir/tst_primary_dir/primary_file1",
	  "in primary1";
	append_to_file "$test_primary_datadir/tst_primary_dir/primary_file2",
	  "in primary2";
	mkdir "$test_primary_datadir/tst_primary_dir/primary_subdir/";
	append_to_file
	  "$test_primary_datadir/tst_primary_dir/primary_subdir/primary_file3",
	  "in primary3";

	RewindTest::promote_standby();
	RewindTest::run_pg_rewind($test_mode);

	# List files in the data directory after rewind. All the files that
	# were present in the standby should be present after rewind, and
	# all the files that were added on the primary should be removed.
	my @paths;
	find(
		sub {
			push @paths, $File::Find::name
			  if $File::Find::name =~ m/.*tst_.*/;
		},
		$test_primary_datadir);
	@paths = sort @paths;
	is_deeply(
		\@paths,
		[
			"$test_primary_datadir/tst_both_dir",
			"$test_primary_datadir/tst_both_dir/both_file1",
			"$test_primary_datadir/tst_both_dir/both_file2",
			"$test_primary_datadir/tst_both_dir/both_subdir",
			"$test_primary_datadir/tst_both_dir/both_subdir/both_file3",
			"$test_primary_datadir/tst_standby_dir",
			"$test_primary_datadir/tst_standby_dir/standby_file1",
			"$test_primary_datadir/tst_standby_dir/standby_file2",
			"$test_primary_datadir/tst_standby_dir/standby_file3 with 'quotes'",
			"$test_primary_datadir/tst_standby_dir/standby_subdir",
			"$test_primary_datadir/tst_standby_dir/standby_subdir/standby_file4"
		],
		"file lists match");

	RewindTest::clean_rewind_test();
	return;
}

# Run the test in both modes.
run_test('local');
run_test('remote');

done_testing();
