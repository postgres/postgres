# Test how pg_rewind reacts to extra files and directories in the data dirs.

use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

use File::Find;

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;


sub run_test
{
	my $test_mode = shift;

	RewindTest::setup_cluster($test_mode);
	RewindTest::start_master();

	my $test_master_datadir = $node_master->data_dir;

	# Create a subdir and files that will be present in both
	mkdir "$test_master_datadir/tst_both_dir";
	append_to_file "$test_master_datadir/tst_both_dir/both_file1", "in both1";
	append_to_file "$test_master_datadir/tst_both_dir/both_file2", "in both2";
	mkdir "$test_master_datadir/tst_both_dir/both_subdir/";
	append_to_file "$test_master_datadir/tst_both_dir/both_subdir/both_file3",
	  "in both3";

	RewindTest::create_standby($test_mode);

	# Create different subdirs and files in master and standby
	my $test_standby_datadir = $node_standby->data_dir;

	mkdir "$test_standby_datadir/tst_standby_dir";
	append_to_file "$test_standby_datadir/tst_standby_dir/standby_file1",
	  "in standby1";
	append_to_file "$test_standby_datadir/tst_standby_dir/standby_file2",
	  "in standby2";
	mkdir "$test_standby_datadir/tst_standby_dir/standby_subdir/";
	append_to_file
	  "$test_standby_datadir/tst_standby_dir/standby_subdir/standby_file3",
	  "in standby3";

	mkdir "$test_master_datadir/tst_master_dir";
	append_to_file "$test_master_datadir/tst_master_dir/master_file1",
	  "in master1";
	append_to_file "$test_master_datadir/tst_master_dir/master_file2",
	  "in master2";
	mkdir "$test_master_datadir/tst_master_dir/master_subdir/";
	append_to_file
	  "$test_master_datadir/tst_master_dir/master_subdir/master_file3",
	  "in master3";

	RewindTest::promote_standby();
	RewindTest::run_pg_rewind($test_mode);

	# List files in the data directory after rewind.
	my @paths;
	find(
		sub {
			push @paths, $File::Find::name
			  if $File::Find::name =~ m/.*tst_.*/;
		},
		$test_master_datadir);
	@paths = sort @paths;

	# File::Find converts backslashes to slashes in the newer Perl
	# versions. To support all Perl versions, do the same conversion
	# for Windows before comparing the paths.
	if ($windows_os)
	{
		for my $filename (@paths)
		{
			$filename =~ s{\\}{/}g;
		}
		$test_master_datadir =~ s{\\}{/}g;
	}

	is_deeply(
		\@paths,
		[
			"$test_master_datadir/tst_both_dir",
			"$test_master_datadir/tst_both_dir/both_file1",
			"$test_master_datadir/tst_both_dir/both_file2",
			"$test_master_datadir/tst_both_dir/both_subdir",
			"$test_master_datadir/tst_both_dir/both_subdir/both_file3",
			"$test_master_datadir/tst_standby_dir",
			"$test_master_datadir/tst_standby_dir/standby_file1",
			"$test_master_datadir/tst_standby_dir/standby_file2",
			"$test_master_datadir/tst_standby_dir/standby_subdir",
			"$test_master_datadir/tst_standby_dir/standby_subdir/standby_file3"
		],
		"file lists match");

	RewindTest::clean_rewind_test();
	return;
}

# Run the test in both modes.
run_test('local');
run_test('remote');

exit(0);
