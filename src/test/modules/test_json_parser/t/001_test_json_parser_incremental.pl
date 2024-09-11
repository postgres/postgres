
# Copyright (c) 2024, PostgreSQL Global Development Group

# Test the incremental (table-driven) json parser.


use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;

my $test_file = "$FindBin::RealBin/../tiny.json";

my @exes =
  ("test_json_parser_incremental", "test_json_parser_incremental_shlib");

foreach my $exe (@exes)
{
	note "testing executable $exe";

	# Test the  usage error
	my ($stdout, $stderr) = run_command([ $exe, "-c", 10 ]);
	like($stderr, qr/Usage:/, 'error message if not enough arguments');

	# Test that we get success for small chunk sizes from 64 down to 1.
	for (my $size = 64; $size > 0; $size--)
	{
		($stdout, $stderr) = run_command([ $exe, "-c", $size, $test_file ]);

		like($stdout, qr/SUCCESS/, "chunk size $size: test succeeds");
		is($stderr, "", "chunk size $size: no error output");
	}
}

done_testing();
