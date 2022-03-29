
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
$node->init(extra => [ '--locale=C', '--encoding=UTF8' ]);
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

# test \timing
psql_like(
	$node,
	'\timing on
SELECT 1',
	qr/^1$
^Time: \d+.\d\d\d ms/m,
	'\timing');

# test that ENCODING variable is set and that it is updated when
# client encoding is changed
psql_like(
	$node,
	'\echo :ENCODING
set client_encoding = LATIN1;
\echo :ENCODING',
	qr/^UTF8$
^LATIN1$/m,
	'ENCODING variable is set and updated');

# test LISTEN/NOTIFY
psql_like(
	$node,
	'LISTEN foo;
NOTIFY foo;',
	qr/^Asynchronous notification "foo" received from server process with PID \d+\.$/,
	'notification');

psql_like(
	$node,
	"LISTEN foo;
NOTIFY foo, 'bar';",
	qr/^Asynchronous notification "foo" with payload "bar" received from server process with PID \d+\.$/,
	'notification with payload');

# test behavior and output on server crash
my ($ret, $out, $err) = $node->psql(
	'postgres',
	"SELECT 'before' AS running;\n" .
	"SELECT pg_terminate_backend(pg_backend_pid());\n" .
	"SELECT 'AFTER' AS not_running;\n");

is($ret, 2, 'server crash: psql exit code');
like($out, qr/before/, 'server crash: output before crash');
ok($out !~ qr/AFTER/, 'server crash: no output after crash');
is($err, 'psql:<stdin>:2: FATAL:  terminating connection due to administrator command
server closed the connection unexpectedly
	This probably means the server terminated abnormally
	before or while processing the request.
psql:<stdin>:2: fatal: connection to server was lost',
	'server crash: error message');

done_testing();
