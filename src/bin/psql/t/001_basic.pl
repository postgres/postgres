
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
	my ($ret, $stdout, $stderr) =
	  $node->psql('postgres', $sql, replication => 'database');

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

psql_like($node, '\copyright',   qr/Copyright/, '\copyright');
psql_like($node, '\help',        qr/ALTER/,     '\help without arguments');
psql_like($node, '\help SELECT', qr/SELECT/,    '\help with argument');

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
^Time: \d+[.,]\d\d\d ms/m,
	'\timing with successful query');

# test \timing with query that fails
{
	my ($ret, $stdout, $stderr) =
	  $node->psql('postgres', "\\timing on\nSELECT error");
	isnt($ret, 0, '\timing with query error: query failed');
	like(
		$stdout,
		qr/^Time: \d+[.,]\d\d\d ms/m,
		'\timing with query error: timing output appears');
	unlike(
		$stdout,
		qr/^Time: 0[.,]000 ms/m,
		'\timing with query error: timing was updated');
}

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
my ($ret, $out, $err) = $node->psql('postgres',
	    "SELECT 'before' AS running;\n"
	  . "SELECT pg_terminate_backend(pg_backend_pid());\n"
	  . "SELECT 'AFTER' AS not_running;\n");

is($ret, 2, 'server crash: psql exit code');
like($out, qr/before/, 'server crash: output before crash');
ok($out !~ qr/AFTER/, 'server crash: no output after crash');
is( $err,
	'psql:<stdin>:2: FATAL:  terminating connection due to administrator command
psql:<stdin>:2: server closed the connection unexpectedly
	This probably means the server terminated abnormally
	before or while processing the request.
psql:<stdin>:2: error: connection to server was lost',
	'server crash: error message');

# test \errverbose
#
# (This is not in the regular regression tests because the output
# contains the source code location and we don't want to have to
# update that every time it changes.)

psql_like(
	$node,
	'SELECT 1;
\errverbose',
	qr/^1\nThere is no previous error\.$/,
	'\errverbose with no previous error');

# There are three main ways to run a query that might affect
# \errverbose: The normal way, using a cursor by setting FETCH_COUNT,
# and using \gdesc.  Test them all.

like(
	(   $node->psql(
			'postgres',
			"SELECT error;\n\\errverbose",
			on_error_stop => 0))[2],
	qr/\A^psql:<stdin>:1: ERROR:  .*$
^LINE 1: SELECT error;$
^ *^.*$
^psql:<stdin>:2: error: ERROR:  [0-9A-Z]{5}: .*$
^LINE 1: SELECT error;$
^ *^.*$
^LOCATION: .*$/m,
	'\errverbose after normal query with error');

like(
	(   $node->psql(
			'postgres',
			"\\set FETCH_COUNT 1\nSELECT error;\n\\errverbose",
			on_error_stop => 0))[2],
	qr/\A^psql:<stdin>:2: ERROR:  .*$
^LINE 2: SELECT error;$
^ *^.*$
^psql:<stdin>:3: error: ERROR:  [0-9A-Z]{5}: .*$
^LINE 2: SELECT error;$
^ *^.*$
^LOCATION: .*$/m,
	'\errverbose after FETCH_COUNT query with error');

like(
	(   $node->psql(
			'postgres',
			"SELECT error\\gdesc\n\\errverbose",
			on_error_stop => 0))[2],
	qr/\A^psql:<stdin>:1: ERROR:  .*$
^LINE 1: SELECT error$
^ *^.*$
^psql:<stdin>:2: error: ERROR:  [0-9A-Z]{5}: .*$
^LINE 1: SELECT error$
^ *^.*$
^LOCATION: .*$/m,
	'\errverbose after \gdesc with error');

# Check behavior when using multiple -c and -f switches.
# Note that we cannot test backend-side errors as tests are unstable in this
# case: IPC::Run can complain about a SIGPIPE if psql quits before reading a
# query result.
my $tempdir = PostgreSQL::Test::Utils::tempdir;
$node->safe_psql('postgres', "CREATE TABLE tab_psql_single (a int);");

# Tests with ON_ERROR_STOP.
$node->command_ok(
	[
		'psql',                                   '-X',
		'--single-transaction',                   '-v',
		'ON_ERROR_STOP=1',                        '-c',
		'INSERT INTO tab_psql_single VALUES (1)', '-c',
		'INSERT INTO tab_psql_single VALUES (2)'
	],
	'ON_ERROR_STOP, --single-transaction and multiple -c switches');
my $row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '2',
	'--single-transaction commits transaction, ON_ERROR_STOP and multiple -c switches'
);

$node->command_fails(
	[
		'psql',                                   '-X',
		'--single-transaction',                   '-v',
		'ON_ERROR_STOP=1',                        '-c',
		'INSERT INTO tab_psql_single VALUES (3)', '-c',
		"\\copy tab_psql_single FROM '$tempdir/nonexistent'"
	],
	'ON_ERROR_STOP, --single-transaction and multiple -c switches, error');
$row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '2',
	'client-side error rolls back transaction, ON_ERROR_STOP and multiple -c switches'
);

# Tests mixing files and commands.
my $copy_sql_file   = "$tempdir/tab_copy.sql";
my $insert_sql_file = "$tempdir/tab_insert.sql";
append_to_file($copy_sql_file,
	"\\copy tab_psql_single FROM '$tempdir/nonexistent';");
append_to_file($insert_sql_file, 'INSERT INTO tab_psql_single VALUES (4);');
$node->command_ok(
	[
		'psql',            '-X', '--single-transaction', '-v',
		'ON_ERROR_STOP=1', '-f', $insert_sql_file,       '-f',
		$insert_sql_file
	],
	'ON_ERROR_STOP, --single-transaction and multiple -f switches');
$row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '4',
	'--single-transaction commits transaction, ON_ERROR_STOP and multiple -f switches'
);

$node->command_fails(
	[
		'psql',            '-X', '--single-transaction', '-v',
		'ON_ERROR_STOP=1', '-f', $insert_sql_file,       '-f',
		$copy_sql_file
	],
	'ON_ERROR_STOP, --single-transaction and multiple -f switches, error');
$row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '4',
	'client-side error rolls back transaction, ON_ERROR_STOP and multiple -f switches'
);

# Tests without ON_ERROR_STOP.
# The last switch fails on \copy.  The command returns a failure and the
# transaction commits.
$node->command_fails(
	[
		'psql',                 '-X',
		'--single-transaction', '-f',
		$insert_sql_file,       '-f',
		$insert_sql_file,       '-c',
		"\\copy tab_psql_single FROM '$tempdir/nonexistent'"
	],
	'no ON_ERROR_STOP, --single-transaction and multiple -f/-c switches');
$row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '6',
	'client-side error commits transaction, no ON_ERROR_STOP and multiple -f/-c switches'
);

# The last switch fails on \copy coming from an input file.  The command
# returns a success and the transaction commits.
$node->command_ok(
	[
		'psql',           '-X', '--single-transaction', '-f',
		$insert_sql_file, '-f', $insert_sql_file,       '-f',
		$copy_sql_file
	],
	'no ON_ERROR_STOP, --single-transaction and multiple -f switches');
$row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '8',
	'client-side error commits transaction, no ON_ERROR_STOP and multiple -f switches'
);

# The last switch makes the command return a success, and the contents of
# the transaction commit even if there is a failure in-between.
$node->command_ok(
	[
		'psql',                                   '-X',
		'--single-transaction',                   '-c',
		'INSERT INTO tab_psql_single VALUES (5)', '-f',
		$copy_sql_file,                           '-c',
		'INSERT INTO tab_psql_single VALUES (6)'
	],
	'no ON_ERROR_STOP, --single-transaction and multiple -c switches');
$row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '10',
	'client-side error commits transaction, no ON_ERROR_STOP and multiple -c switches'
);

done_testing();
