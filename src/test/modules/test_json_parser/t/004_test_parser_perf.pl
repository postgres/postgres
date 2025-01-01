
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test the JSON parser performance tester. Here we are just checking that
# the performance tester can run, both with the standard parser and the
# incremental parser. An actual performance test will run with thousands
# of iterations instead of just one.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;

use File::Temp qw(tempfile);

my $test_file = "$FindBin::RealBin/../tiny.json";

my $exe = "test_json_parser_perf";

my $contents = slurp_file($test_file);

my $dir = PostgreSQL::Test::Utils::tempdir;
my ($fh, $fname) = tempfile(DIR => $dir);

# repeat the input json file 50 times in an array

print $fh, '[', $contents, ",$contents" x 49, ']';

close($fh);

# but only do one iteration

my ($result) = run_log([ $exe, "1", $fname ]);

ok($result == 0, "perf test runs with recursive descent parser");

$result = run_log([ $exe, "-i", "1", $fname ]);

ok($result == 0, "perf test runs with table driven parser");

done_testing();
