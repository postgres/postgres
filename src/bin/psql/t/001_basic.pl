
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use locale;

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

	is($ret, 0, "$test_name: exit code 0");
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

	$result = IPC::Run::run [ 'psql', "--help=$arg" ],
	  '>' => \$stdout,
	  '2>' => \$stderr;
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
# \errverbose: The normal way, piecemeal retrieval using FETCH_COUNT,
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
^LINE 1: SELECT error;$
^ *^.*$
^psql:<stdin>:3: error: ERROR:  [0-9A-Z]{5}: .*$
^LINE 1: SELECT error;$
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
		'psql',
		'--no-psqlrc',
		'--single-transaction',
		'--set' => 'ON_ERROR_STOP=1',
		'--command' => 'INSERT INTO tab_psql_single VALUES (1)',
		'--command' => 'INSERT INTO tab_psql_single VALUES (2)',
	],
	'ON_ERROR_STOP, --single-transaction and multiple -c switches');
my $row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '2',
	'--single-transaction commits transaction, ON_ERROR_STOP and multiple -c switches'
);

$node->command_fails(
	[
		'psql',
		'--no-psqlrc',
		'--single-transaction',
		'--set' => 'ON_ERROR_STOP=1',
		'--command' => 'INSERT INTO tab_psql_single VALUES (3)',
		'--command' => "\\copy tab_psql_single FROM '$tempdir/nonexistent'"
	],
	'ON_ERROR_STOP, --single-transaction and multiple -c switches, error');
$row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '2',
	'client-side error rolls back transaction, ON_ERROR_STOP and multiple -c switches'
);

# Tests mixing files and commands.
my $copy_sql_file = "$tempdir/tab_copy.sql";
my $insert_sql_file = "$tempdir/tab_insert.sql";
append_to_file($copy_sql_file,
	"\\copy tab_psql_single FROM '$tempdir/nonexistent';");
append_to_file($insert_sql_file, 'INSERT INTO tab_psql_single VALUES (4);');
$node->command_ok(
	[
		'psql',
		'--no-psqlrc',
		'--single-transaction',
		'--set' => 'ON_ERROR_STOP=1',
		'--file' => $insert_sql_file,
		'--file' => $insert_sql_file
	],
	'ON_ERROR_STOP, --single-transaction and multiple -f switches');
$row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '4',
	'--single-transaction commits transaction, ON_ERROR_STOP and multiple -f switches'
);

$node->command_fails(
	[
		'psql',
		'--no-psqlrc',
		'--single-transaction',
		'--set' => 'ON_ERROR_STOP=1',
		'--file' => $insert_sql_file,
		'--file' => $copy_sql_file
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
		'psql',
		'--no-psqlrc',
		'--single-transaction',
		'--file' => $insert_sql_file,
		'--file' => $insert_sql_file,
		'--command' => "\\copy tab_psql_single FROM '$tempdir/nonexistent'"
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
		'psql',
		'--no-psqlrc',
		'--single-transaction',
		'--file' => $insert_sql_file,
		'--file' => $insert_sql_file,
		'--file' => $copy_sql_file
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
		'psql',
		'--no-psqlrc',
		'--single-transaction',
		'--command' => 'INSERT INTO tab_psql_single VALUES (5)',
		'--file' => $copy_sql_file,
		'--command' => 'INSERT INTO tab_psql_single VALUES (6)'
	],
	'no ON_ERROR_STOP, --single-transaction and multiple -c switches');
$row_count =
  $node->safe_psql('postgres', 'SELECT count(*) FROM tab_psql_single');
is($row_count, '10',
	'client-side error commits transaction, no ON_ERROR_STOP and multiple -c switches'
);

# Test \copy from with DEFAULT option
$node->safe_psql(
	'postgres',
	"CREATE TABLE copy_default (
		id integer PRIMARY KEY,
		text_value text NOT NULL DEFAULT 'test',
		ts_value timestamp without time zone NOT NULL DEFAULT '2022-07-05'
	)"
);

my $copy_default_sql_file = "$tempdir/copy_default.csv";
append_to_file($copy_default_sql_file, "1,value,2022-07-04\n");
append_to_file($copy_default_sql_file, "2,placeholder,2022-07-03\n");
append_to_file($copy_default_sql_file, "3,placeholder,placeholder\n");

psql_like(
	$node,
	"\\copy copy_default from $copy_default_sql_file with (format 'csv', default 'placeholder');
	SELECT * FROM copy_default",
	qr/1\|value\|2022-07-04 00:00:00
2|test|2022-07-03 00:00:00
3|test|2022-07-05 00:00:00/,
	'\copy from with DEFAULT');

# Check \watch
# Note: the interval value is parsed with locale-aware strtod()
psql_like(
	$node, sprintf('SELECT 1 \watch c=3 i=%g', 0.01),
	qr/1\n1\n1/, '\watch with 3 iterations, interval of 0.01');

# Sub-millisecond wait works, equivalent to 0.
psql_like(
	$node, sprintf('SELECT 1 \watch c=3 i=%g', 0.0001),
	qr/1\n1\n1/, '\watch with 3 iterations, interval of 0.0001');

# Check \watch minimum row count
psql_fails_like(
	$node,
	'SELECT 3 \watch m=x',
	qr/incorrect minimum row count/,
	'\watch, invalid minimum row setting');

psql_fails_like(
	$node,
	'SELECT 3 \watch m=1 min_rows=2',
	qr/minimum row count specified more than once/,
	'\watch, minimum rows is specified more than once');

psql_like(
	$node,
	sprintf(
		q{with x as (
		select now()-backend_start AS howlong
		from pg_stat_activity
		where pid = pg_backend_pid()
	  ) select 123 from x where howlong < '2 seconds' \watch i=%g m=2}, 0.5),
	qr/^123$/,
	'\watch, 2 minimum rows');

# Check \watch errors
psql_fails_like(
	$node,
	'SELECT 1 \watch -10',
	qr/incorrect interval value "-10"/,
	'\watch, negative interval');
psql_fails_like(
	$node,
	'SELECT 1 \watch 10ab',
	qr/incorrect interval value "10ab"/,
	'\watch, incorrect interval');
psql_fails_like(
	$node,
	'SELECT 1 \watch 10e400',
	qr/incorrect interval value "10e400"/,
	'\watch, out-of-range interval');
psql_fails_like(
	$node,
	'SELECT 1 \watch 1 1',
	qr/interval value is specified more than once/,
	'\watch, interval value is specified more than once');
psql_fails_like(
	$node,
	'SELECT 1 \watch c=1 c=1',
	qr/iteration count is specified more than once/,
	'\watch, iteration count is specified more than once');

# Test \g output piped into a program.
# The program is perl -pe '' to simply copy the input to the output.
my $g_file = "$tempdir/g_file_1.out";
my $perlbin = $^X;
$perlbin =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;
my $pipe_cmd = "$perlbin -pe '' >$g_file";

psql_like($node, "SELECT 'one' \\g | $pipe_cmd", qr//, "one command \\g");
my $c1 = slurp_file($g_file);
like($c1, qr/one/);

psql_like($node, "SELECT 'two' \\; SELECT 'three' \\g | $pipe_cmd",
	qr//, "two commands \\g");
my $c2 = slurp_file($g_file);
like($c2, qr/two.*three/s);


psql_like(
	$node,
	"\\set SHOW_ALL_RESULTS 0\nSELECT 'four' \\; SELECT 'five' \\g | $pipe_cmd",
	qr//,
	"two commands \\g with only last result");
my $c3 = slurp_file($g_file);
like($c3, qr/five/);
unlike($c3, qr/four/);

psql_like($node, "copy (values ('foo'),('bar')) to stdout \\g | $pipe_cmd",
	qr//, "copy output passed to \\g pipe");
my $c4 = slurp_file($g_file);
like($c4, qr/foo.*bar/s);

done_testing();
