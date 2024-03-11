
use strict;
use warnings;

use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;

use File::Temp qw(tempfile);

my $test_file = "$FindBin::RealBin/../tiny.json";

my $exe = "test_json_parser_incremental";

for (my $size = 64; $size > 0; $size--)
{
	my ($stdout, $stderr) = run_command( [$exe, "-c", $size, $test_file] );

	like($stdout, qr/SUCCESS/, "chunk size $size: test succeeds");
	is($stderr, "", "chunk size $size: no error output");
}

done_testing();
