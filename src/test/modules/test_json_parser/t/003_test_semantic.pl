use strict;
use warnings;

use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;

use File::Temp qw(tempfile);

my $test_file = "$FindBin::RealBin/../tiny.json";
my $test_out = "$FindBin::RealBin/../tiny.out";

my $exe = "test_json_parser_incremental";

my ($stdout, $stderr) = run_command( [$exe, "-s", $test_file] );

is($stderr, "", "no error output");

my ($fh, $fname) = tempfile();

print $fh $stdout,"\n";

close($fh);

($stdout, $stderr) = run_command(["diff", "-u", $fname, $test_out]);

is($stdout, "", "no output diff");
is($stderr, "", "no diff error");

done_testing();
