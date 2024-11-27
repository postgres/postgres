
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test the incremental JSON parser with semantic routines, and compare the
# output with the expected output.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;

use File::Temp qw(tempfile);

my $test_file = "$FindBin::RealBin/../tiny.json";
my $test_out = "$FindBin::RealBin/../tiny.out";

my @exes = (
	[ "test_json_parser_incremental", ],
	[ "test_json_parser_incremental", "-o", ],
	[ "test_json_parser_incremental_shlib", ],
	[ "test_json_parser_incremental_shlib", "-o", ]);

foreach my $exe (@exes)
{
	note "testing executable @$exe";

	my ($stdout, $stderr) = run_command([ @$exe, "-s", $test_file ]);

	is($stderr, "", "no error output");

	my $dir = PostgreSQL::Test::Utils::tempdir;
	my ($fh, $fname) = tempfile(DIR => $dir);

	print $fh $stdout, "\n";

	close($fh);

	my @diffopts = ("-u");
	push(@diffopts, "--strip-trailing-cr") if $windows_os;
	($stdout, $stderr) =
	  run_command([ "diff", @diffopts, $fname, $test_out ]);

	is($stdout, "", "no output diff");
	is($stderr, "", "no diff error");
}

done_testing();
