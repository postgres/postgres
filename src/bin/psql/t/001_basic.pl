
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('psql');
program_version_ok('psql');
program_options_handling_ok('psql');

my ($stdout, $stderr);
my $result;

# Execute a psql command and check its result patterns.
sub psql_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $node            = shift;
	my $test_name       = shift;
	my $query           = shift;
	my $expected_stdout = shift;
	my $expected_stderr = shift;

	die "cannot specify both expected stdout and stderr here"
	  if (defined($expected_stdout) && defined($expected_stderr));

	# Use the context of a WAL sender, some of the tests rely on that.
	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres', $query,
		on_error_die => 0,
		replication  => 'database');

	if (defined($expected_stdout))
	{
		is($ret,    0,  "$test_name: expected result code");
		is($stderr, '', "$test_name: no stderr");
		like($stdout, $expected_stdout, "$test_name: stdout matches");
	}
	if (defined($expected_stderr))
	{
		isnt($ret, 0, "$test_name: expected result code");
		like($stderr, $expected_stderr, "$test_name: stderr matches");
	}

	return;
}

# test --help=foo, analogous to program_help_ok()
foreach my $arg (qw(commands variables))
{
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

psql_like($node, '\copyright', '\copyright', qr/Copyright/, undef);
psql_like($node, '\help without arguments', '\help', qr/ALTER/, undef);
psql_like($node, '\help with argument', '\help SELECT', qr/SELECT/, undef);

# Test clean handling of unsupported replication command responses
psql_like(
	$node,
	'handling of unexpected PQresultStatus',
	'START_REPLICATION 0/0',
	undef, qr/unexpected PQresultStatus: 8$/);

done_testing();
