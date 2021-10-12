
# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 25;

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
$node->append_conf(
	'postgresql.conf', q{
wal_level = 'logical'
max_replication_slots = 4
max_wal_senders = 4
});
$node->start;

$node->command_like([ 'psql', '-c', '\copyright' ], qr/Copyright/, '\copyright');
$node->command_like([ 'psql', '-c', '\help' ], qr/ALTER/, '\help without arguments');
$node->command_like([ 'psql', '-c', '\help SELECT' ], qr/SELECT/, '\help');


# Test clean handling of unsupported replication command responses
$node->command_fails_like([ 'psql', 'replication=database', '-c', 'START_REPLICATION 0/0' ],
	qr/^unexpected PQresultStatus: 8$/, 'handling of unexpected PQresultStatus');
