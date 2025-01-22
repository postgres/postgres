# pg_bsd_indent: some simple tests

# The test cases come from FreeBSD upstream, but this test scaffolding is ours.
# Copyright (c) 2017-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use Cwd qw(getcwd);
use File::Copy "cp";
use File::Spec;

use PostgreSQL::Test::Utils;
use Test::More;

# We expect to be started in the source directory (even in a VPATH build);
# we want to run pg_bsd_indent in the tmp_check directory to reduce clutter.
# (Also, it's caller's responsibility that pg_bsd_indent be in the PATH.)
my $src_dir = getcwd;
chdir ${PostgreSQL::Test::Utils::tmp_check};

# Basic tests: pg_bsd_indent knows --version but not much else.
program_version_ok('pg_bsd_indent');

# Run pg_bsd_indent on pre-fab test cases.
# Any diffs in the generated files will be accumulated here.
my $diffs_file = "test.diffs";

# options used with diff (see pg_regress.c's pretty_diff_opts)
my @diffopts = ("-U3");
push(@diffopts, "--strip-trailing-cr") if $windows_os;

# Copy support files to current dir, so *.pro files don't need to know path.
while (my $file = glob("$src_dir/tests/*.list"))
{
	cp($file, ".") || die "cp $file failed: $!";
}

while (my $test_src = glob("$src_dir/tests/*.0"))
{
	# extract test basename
	my ($volume, $directories, $test) = File::Spec->splitpath($test_src);
	$test =~ s/\.0$//;
	# run pg_bsd_indent
	command_ok(
		[
			'pg_bsd_indent', $test_src,
			"$test.out", "-P$src_dir/tests/$test.pro"
		],
		"pg_bsd_indent succeeds on $test");
	# check result matches, adding any diff to $diffs_file
	my $result =
	  run_log([ 'diff', @diffopts, "$test_src.stdout", "$test.out" ],
		'>>', $diffs_file);
	ok($result, "pg_bsd_indent output matches for $test");
}

done_testing();
