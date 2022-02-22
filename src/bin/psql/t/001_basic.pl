
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('psql');
program_version_ok('psql');
program_options_handling_ok('psql');

# Execute a psql command and check its output.
sub psql_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $sql, $expected_stdout, $test_name) = @_;

	my ($ret, $stdout, $stderr) = $node->psql('postgres', $sql);

	is($ret,    0,  "$test_name: exit code 0");
	is($stderr, '', "$test_name: no stderr");
	like($stdout, $expected_stdout, "$test_name: matches");

	return;
}

# Execute a psql command and check that it fails and check the stderr.
sub psql_fails_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $sql, $expected_stderr, $test_name) = @_;

	# Use the context of a WAL sender, some of the tests rely on that.
	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres', $sql,
		replication  => 'database');

	isnt($ret, 0, "$test_name: exit code not 0");
	like($stderr, $expected_stderr, "$test_name: matches");

	return;
}

# test --help=foo, analogous to program_help_ok()
foreach my $arg (qw(commands variables))
{
	my ($stdout, $stderr);
	my $result;

	$result = IPC::Run::run [ 'psql', "--help=$arg" ], '>', \$stdout, '2>',
	  \$stderr;
	ok($result, "psql --help=$arg exit code 0");
	isnt($stdout, '', "psql --help=$arg goes to stdout");
	is($stderr, '', "psql --help=$arg nothing to stderr");
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
wal_level = 'logical'
max_replication_slots = 4
max_wal_senders = 4
});
$node->start;

psql_like($node, '\copyright', qr/Copyright/, '\copyright');
psql_like($node, '\help', qr/ALTER/, '\help without arguments');
psql_like($node, '\help SELECT', qr/SELECT/, '\help with argument');

# Test clean handling of unsupported replication command responses
psql_fails_like(
	$node,
	'START_REPLICATION 0/0',
	qr/unexpected PQresultStatus: 8$/,
	'handling of unexpected PQresultStatus');

done_testing();
