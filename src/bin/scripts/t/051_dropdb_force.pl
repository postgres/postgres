#
# Tests the force option of drop database command.
#
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 9;

# To avoid hanging while expecting some specific input from a psql
# instance being driven by us, add a timeout high enough that it
# should never trigger even on very slow machines, unless something
# is really wrong.
my $psql_timeout = IPC::Run::timer(60);

my $node = get_new_node('master');
$node->init;
$node->start;

# Create a database that will be dropped.  This will test that the force
# option works when no other backend is connected to the database being
# dropped.
$node->safe_psql('postgres', 'CREATE DATABASE foobar');
$node->issues_sql_like(
        [ 'dropdb', '--force', 'foobar' ],
        qr/statement: DROP DATABASE foobar WITH \(FORCE\);/,
        'SQL DROP DATABASE (FORCE) run');

# database foobar must not exist.
is( $node->safe_psql(
		'postgres',
		qq[SELECT EXISTS(SELECT * FROM pg_database WHERE datname='foobar');]
	),
	'f',
	'database foobar was removed');

# Create a database that will be dropped.  This will test that the force
# option works when one other backend is connected to the database being
# dropped.
$node->safe_psql('postgres', 'CREATE DATABASE foobar1');

# Run psql, keeping session alive, so we have an alive backend to kill.
my ($killme_stdin, $killme_stdout, $killme_stderr) = ('', '', '');
my $killme = IPC::Run::start(
	[
		'psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-f', '-', '-d',
		$node->connstr('foobar1')
	],
	'<',
	\$killme_stdin,
	'>',
	\$killme_stdout,
	'2>',
	\$killme_stderr,
	$psql_timeout);

# Ensure killme process is active.
$killme_stdin .= q[
SELECT pg_backend_pid();
];
ok( TestLib::pump_until(
		$killme, $psql_timeout, \$killme_stdout, qr/[[:digit:]]+[\r\n]$/m),
	'acquired pid for SIGTERM');
my $pid = $killme_stdout;
chomp($pid);
$killme_stdout = '';
$killme_stderr = '';

# Check the connections on foobar1 database.
is( $node->safe_psql(
		'postgres',
		qq[SELECT pid FROM pg_stat_activity WHERE datname='foobar1' AND pid = $pid;]
	),
	$pid,
	'database foobar1 is used');

# Now drop database with dropdb --force command.
$node->issues_sql_like(
	[ 'dropdb', '--force', 'foobar1' ],
	qr/statement: DROP DATABASE foobar1 WITH \(FORCE\);/,
	'SQL DROP DATABASE (FORCE) run');

# Check that psql sees the killed backend as having been terminated.
$killme_stdin .= q[
SELECT 1;
];
ok( TestLib::pump_until(
		$killme, $psql_timeout, \$killme_stderr,
		qr/FATAL:  terminating connection due to administrator command/m),
	"psql query died successfully after SIGTERM");
$killme_stderr = '';
$killme_stdout = '';
$killme->finish;

# database foobar1 must not exist.
is( $node->safe_psql(
		'postgres',
		qq[SELECT EXISTS(SELECT * FROM pg_database WHERE datname='foobar1');]
	),
	'f',
	'database foobar1 was removed');

$node->stop();
