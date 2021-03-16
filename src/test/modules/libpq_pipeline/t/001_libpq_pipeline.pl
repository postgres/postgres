use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;
use Test::More tests => 8;
use Cwd;

my $node = get_new_node('main');
$node->init;
$node->start;

my $numrows = 10000;
$ENV{PATH} = "$ENV{PATH}:" . getcwd();

my ($out, $err) = run_command(['libpq_pipeline', 'tests']);
die "oops: $err" unless $err eq '';
my @tests = split(/\s+/, $out);

for my $testname (@tests)
{
	$node->command_ok(
		[ 'libpq_pipeline', $testname, $node->connstr('postgres'), $numrows ],
		"libpq_pipeline $testname");
}

$node->stop('fast');
