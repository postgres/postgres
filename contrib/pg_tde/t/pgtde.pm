package PGTDE;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use File::Basename;
use File::Compare;
use Test::More;

# Expected .out filename of TAP testcase being executed. These are already part of repo under t/expected/*.
our $expected_filename_with_path;

# Result .out filename of TAP testcase being executed. Where needed, a new *.out will be created for each TAP test.
our $out_filename_with_path;

# Runtime output file that is used only for debugging purposes for comparison to PGSS, blocks and timings.
our $debug_out_filename_with_path;

my $expected_folder = "t/expected";
my $results_folder = "t/results";

sub psql
{
	my ($node, $dbname, $sql) = @_;

	my (undef, $stdout, $stderr) = $node->psql($dbname, $sql,
		extra_params => [ '-a', '-Pformat=aligned', '-Ptuples_only=off' ]);

	if ($stdout ne '')
	{
		append_to_result_file($stdout);
	}

	if ($stderr ne '')
	{
		append_to_result_file($stderr);
	}
}

sub append_to_result_file
{
	my ($str) = @_;

	append_to_file($out_filename_with_path, $str . "\n");
}

sub append_to_debug_file
{
	my ($str) = @_;

	append_to_file($debug_out_filename_with_path, $str . "\n");
}

sub setup_files_dir
{
	my ($test_filename) = @_;

	unless (-d $results_folder)
	{
		mkdir $results_folder
		  or die "Can't create folder $results_folder: $!\n";
	}

	my ($test_name) = $test_filename =~ /([^.]*)/;

	$expected_filename_with_path = "${expected_folder}/${test_name}.out";
	$out_filename_with_path = "${results_folder}/${test_name}.out";
	$debug_out_filename_with_path =
	  "${results_folder}/${test_name}.out.debug";

	if (-f $out_filename_with_path)
	{
		unlink($out_filename_with_path)
		  or die
		  "Can't delete already existing $out_filename_with_path: $!\n";
	}
}

sub compare_results
{
	return compare($expected_filename_with_path, $out_filename_with_path);
}

1;
