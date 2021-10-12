
# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 23;

program_help_ok('psql');
program_version_ok('psql');
program_options_handling_ok('psql');

my ($stdout, $stderr);
my $result;

# test --help=foo, analogous to program_help_ok()
foreach my $arg (qw(commands variables))
{
	$result = IPC::Run::run [ 'psql', "--help=$arg" ], '>', \$stdout, '2>', \$stderr;
	ok($result, "psql --help=$arg exit code 0");
	isnt($stdout, '', "psql --help=$arg goes to stdout");
	is($stderr, '', "psql --help=$arg nothing to stderr");
}

my $node = PostgresNode->new('main');
$node->init;
$node->start;

$node->command_like([ 'psql', '-c', '\copyright' ], qr/Copyright/, '\copyright');
$node->command_like([ 'psql', '-c', '\help' ], qr/ALTER/, '\help without arguments');
$node->command_like([ 'psql', '-c', '\help SELECT' ], qr/SELECT/, '\help');
