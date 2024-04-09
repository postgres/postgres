
use strict;
use warnings;

use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;

use File::Temp qw(tempfile);

my $test_file = "$FindBin::RealBin/../tiny.json";

my $exe = "test_json_parser_perf";

my $contents = slurp_file($test_file);

my ($fh, $fname) = tempfile(UNLINK => 1);

# repeat the input json file 50 times in an array

print $fh, '[', $contents , ",$contents" x 49 , ']';

close($fh);

# but only do one iteration

my ($result) = run_log([ $exe, "1",  $fname ] );

ok($result == 0, "perf test runs with RD parser");

$result = run_log([ $exe, "-i" , "1",  $fname ]);

ok($result == 0, "perf test runs with table driven parser");

done_testing();
