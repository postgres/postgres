# Copyright (c) 2025, PostgreSQL Global Development Group

# Test 128-bit integer arithmetic code in int128.h

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

# Run the test program with 1M iterations
my $exe = "test_int128";
my $size = 1_000_000;

note "testing executable $exe";

my ($stdout, $stderr) = run_command([ $exe, $size ]);

SKIP:
{
	skip "no native int128 type", 2 if $stdout =~ /skipping tests/;

	is($stdout, "", "test_int128: no stdout");
	is($stderr, "", "test_int128: no stderr");
}

done_testing();
