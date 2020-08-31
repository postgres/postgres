use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;
use Test::More tests => 6;
use Cwd;

my $node = get_new_node('main');
$node->init;
$node->start;

my $port = $node->port;

my $numrows = 10000;
my @tests =
  qw(disallowed_in_batch simple_batch multi_batch batch_abort timings singlerowmode);
$ENV{PATH} = "$ENV{PATH}:" . getcwd();
for my $testname (@tests)
{
	$node->command_ok(
		[ 'testlibpqbatch', 'dbname=postgres', "$numrows", "$testname" ],
		"testlibpqbatch $testname");
}

$node->stop('fast');
