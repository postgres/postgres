# Test remove of temporary files after a crash.
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More;
use Config;

plan tests => 9;


# To avoid hanging while expecting some specific input from a psql
# instance being driven by us, add a timeout high enough that it
# should never trigger even on very slow machines, unless something
# is really wrong.
my $psql_timeout = IPC::Run::timer(60);

my $node = get_new_node('node_crash');
$node->init();
$node->start();

# By default, PostgresNode doesn't restart after crash
# Reduce work_mem to generate temporary file with a few number of rows
$node->safe_psql(
	'postgres',
	q[ALTER SYSTEM SET remove_temp_files_after_crash = on;
				   ALTER SYSTEM SET log_connections = 1;
				   ALTER SYSTEM SET work_mem = '64kB';
				   ALTER SYSTEM SET restart_after_crash = on;
				   SELECT pg_reload_conf();]);

# create table, insert rows
$node->safe_psql(
	'postgres',
	q[CREATE TABLE tab_crash (a integer UNIQUE);]);

# Run psql, keeping session alive, so we have an alive backend to kill.
my ($killme_stdin, $killme_stdout, $killme_stderr) = ('', '', '');
my $killme = IPC::Run::start(
	[
		'psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-f', '-', '-d',
		$node->connstr('postgres')
	],
	'<',
	\$killme_stdin,
	'>',
	\$killme_stdout,
	'2>',
	\$killme_stderr,
	$psql_timeout);

# Get backend pid
$killme_stdin .= q[
SELECT pg_backend_pid();
];
ok(pump_until($killme, \$killme_stdout, qr/[[:digit:]]+[\r\n]$/m),
	'acquired pid for SIGKILL');
my $pid = $killme_stdout;
chomp($pid);
$killme_stdout = '';
$killme_stderr = '';

# Open a 2nd session that will block the 1st one, using the UNIQUE constraint.
# This will prevent removal of the temporary file created by the 1st session.
my ($killme_stdin2, $killme_stdout2, $killme_stderr2) = ('', '', '');
my $killme2 = IPC::Run::start(
	[
		'psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-f', '-', '-d',
		$node->connstr('postgres')
	],
	'<',
	\$killme_stdin2,
	'>',
	\$killme_stdout2,
	'2>',
	\$killme_stderr2,
	$psql_timeout);

# Insert one tuple and leave the transaction open
$killme_stdin2 .= q[
BEGIN;
SELECT $$insert-tuple-to-lock-next-insert$$;
INSERT INTO tab_crash (a) VALUES(1);
];
pump_until($killme2, \$killme_stdout2, qr/insert-tuple-to-lock-next-insert/m);
$killme_stdout2 = '';
$killme_stderr2 = '';

# Run the query that generates a temporary file and that will be killed before
# it finishes. Since the query that generates the temporary file does not
# return before the connection is killed, use a SELECT before to trigger
# pump_until.
$killme_stdin .= q[
BEGIN;
SELECT $$in-progress-before-sigkill$$;
INSERT INTO tab_crash (a) SELECT i FROM generate_series(1, 5000) s(i);
];
ok(pump_until($killme, \$killme_stdout, qr/in-progress-before-sigkill/m),
	'insert in-progress-before-sigkill');
$killme_stdout = '';
$killme_stderr = '';

# Kill with SIGKILL
my $ret = TestLib::system_log('pg_ctl', 'kill', 'KILL', $pid);
is($ret, 0, 'killed process with KILL');

# Close psql session
$killme->finish;
$killme2->finish;

# Wait till server restarts
$node->poll_query_until('postgres', 'SELECT 1', '1');

# Check for temporary files
is($node->safe_psql(
	'postgres',
	'SELECT COUNT(1) FROM pg_ls_dir($$base/pgsql_tmp$$)'),
	qq(0), 'no temporary files');

#
# Test old behavior (don't remove temporary files after crash)
#
$node->safe_psql(
	'postgres',
	q[ALTER SYSTEM SET remove_temp_files_after_crash = off;
				   SELECT pg_reload_conf();]);

# Restart psql session
($killme_stdin, $killme_stdout, $killme_stderr) = ('', '', '');
$killme->run();

# Get backend pid
$killme_stdin .= q[
SELECT pg_backend_pid();
];
ok(pump_until($killme, \$killme_stdout, qr/[[:digit:]]+[\r\n]$/m),
	'acquired pid for SIGKILL');
$pid = $killme_stdout;
chomp($pid);
$killme_stdout = '';
$killme_stderr = '';

# Restart the 2nd psql session
($killme_stdin2, $killme_stdout2, $killme_stderr2) = ('', '', '');
$killme2->run();

# Insert one tuple and leave the transaction open
$killme_stdin2 .= q[
BEGIN;
SELECT $$insert-tuple-to-lock-next-insert$$;
INSERT INTO tab_crash (a) VALUES(1);
];
pump_until($killme2, \$killme_stdout2, qr/insert-tuple-to-lock-next-insert/m);
$killme_stdout2 = '';
$killme_stderr2 = '';

# Run the query that generates a temporary file and that will be killed before
# it finishes. Since the query that generates the temporary file does not
# return before the connection is killed, use a SELECT before to trigger
# pump_until.
$killme_stdin .= q[
BEGIN;
SELECT $$in-progress-before-sigkill$$;
INSERT INTO tab_crash (a) SELECT i FROM generate_series(1, 5000) s(i);
];
ok(pump_until($killme, \$killme_stdout, qr/in-progress-before-sigkill/m),
	'insert in-progress-before-sigkill');
$killme_stdout = '';
$killme_stderr = '';

# Kill with SIGKILL
$ret = TestLib::system_log('pg_ctl', 'kill', 'KILL', $pid);
is($ret, 0, 'killed process with KILL');

# Close psql session
$killme->finish;
$killme2->finish;

# Wait till server restarts
$node->poll_query_until('postgres', 'SELECT 1', '1');

# Check for temporary files -- should be there
is($node->safe_psql(
	'postgres',
	'SELECT COUNT(1) FROM pg_ls_dir($$base/pgsql_tmp$$)'),
	qq(1), 'one temporary file');

# Restart should remove the temporary files
$node->restart();

# Check the temporary files -- should be gone
is($node->safe_psql(
	'postgres',
	'SELECT COUNT(1) FROM pg_ls_dir($$base/pgsql_tmp$$)'),
	qq(0), 'temporary file was removed');

$node->stop();

# Pump until string is matched, or timeout occurs
sub pump_until
{
	my ($proc, $stream, $untl) = @_;
	$proc->pump_nb();
	while (1)
	{
		last if $$stream =~ /$untl/;
		if ($psql_timeout->is_expired)
		{
			diag("aborting wait: program timed out");
			diag("stream contents: >>", $$stream, "<<");
			diag("pattern searched for: ", $untl);

			return 0;
		}
		if (not $proc->pumpable())
		{
			diag("aborting wait: program died");
			diag("stream contents: >>", $$stream, "<<");
			diag("pattern searched for: ", $untl);

			return 0;
		}
		$proc->pump();
	}
	return 1;
}
