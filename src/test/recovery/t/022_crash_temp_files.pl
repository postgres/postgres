
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test remove of temporary files after a crash.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Config;

if ($Config{osname} eq 'MSWin32')
{
	plan skip_all => 'tests hang on Windows';
	exit;
}

my $psql_timeout = IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default);

my $node = PostgreSQL::Test::Cluster->new('node_crash');
$node->init();
$node->start();

# By default, PostgreSQL::Test::Cluster doesn't restart after crash
# Reduce work_mem to generate temporary file with a few number of rows
$node->safe_psql(
	'postgres',
	q[ALTER SYSTEM SET remove_temp_files_after_crash = on;
				   ALTER SYSTEM SET log_connections = 1;
				   ALTER SYSTEM SET work_mem = '64kB';
				   ALTER SYSTEM SET restart_after_crash = on;
				   SELECT pg_reload_conf();]);

# create table, insert rows
$node->safe_psql('postgres', q[CREATE TABLE tab_crash (a integer UNIQUE);]);

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
ok( pump_until(
		$killme, $psql_timeout, \$killme_stdout, qr/[[:digit:]]+[\r\n]$/m),
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
INSERT INTO tab_crash (a) VALUES(1);
SELECT $$insert-tuple-to-lock-next-insert$$;
];
pump_until($killme2, $psql_timeout, \$killme_stdout2,
	qr/insert-tuple-to-lock-next-insert/m);
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
ok( pump_until(
		$killme, $psql_timeout,
		\$killme_stdout, qr/in-progress-before-sigkill/m),
	'insert in-progress-before-sigkill');
$killme_stdout = '';
$killme_stderr = '';

# Wait until the batch insert gets stuck on the lock.
$killme_stdin2 .= q[
DO $c$
DECLARE
  c INT;
BEGIN
  LOOP
    SELECT COUNT(*) INTO c FROM pg_locks WHERE pid = ] . $pid
  . q[ AND NOT granted;
    IF c > 0 THEN
      EXIT;
    END IF;
  END LOOP;
END; $c$;
SELECT $$insert-tuple-lock-waiting$$;
];

pump_until($killme2, $psql_timeout, \$killme_stdout2,
	qr/insert-tuple-lock-waiting/m);
$killme_stdout2 = '';
$killme_stderr2 = '';

# Kill with SIGKILL
my $ret = PostgreSQL::Test::Utils::system_log('pg_ctl', 'kill', 'KILL', $pid);
is($ret, 0, 'killed process with KILL');

# Close that psql session
$killme->finish;

# Wait till the other session reports failure, ensuring that the postmaster
# has noticed its dead child and begun a restart cycle.
$killme_stdin2 .= qq[
SELECT pg_sleep($PostgreSQL::Test::Utils::timeout_default);
];
ok( pump_until(
		$killme2,
		$psql_timeout,
		\$killme_stderr2,
		qr/WARNING:  terminating connection because of crash of another server process|server closed the connection unexpectedly|connection to server was lost|could not send data to server/m
	),
	"second psql session died successfully after SIGKILL");
$killme2->finish;

# Wait till server finishes restarting
$node->poll_query_until('postgres', undef, '');

# Check for temporary files
is( $node->safe_psql(
		'postgres', 'SELECT COUNT(1) FROM pg_ls_dir($$base/pgsql_tmp$$)'),
	qq(0),
	'no temporary files');

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
ok( pump_until(
		$killme, $psql_timeout, \$killme_stdout, qr/[[:digit:]]+[\r\n]$/m),
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
INSERT INTO tab_crash (a) VALUES(1);
SELECT $$insert-tuple-to-lock-next-insert$$;
];
pump_until($killme2, $psql_timeout, \$killme_stdout2,
	qr/insert-tuple-to-lock-next-insert/m);
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
ok( pump_until(
		$killme, $psql_timeout,
		\$killme_stdout, qr/in-progress-before-sigkill/m),
	'insert in-progress-before-sigkill');
$killme_stdout = '';
$killme_stderr = '';

# Wait until the batch insert gets stuck on the lock.
$killme_stdin2 .= q[
DO $c$
DECLARE
  c INT;
BEGIN
  LOOP
    SELECT COUNT(*) INTO c FROM pg_locks WHERE pid = ] . $pid
  . q[ AND NOT granted;
    IF c > 0 THEN
      EXIT;
    END IF;
  END LOOP;
END; $c$;
SELECT $$insert-tuple-lock-waiting$$;
];

pump_until($killme2, $psql_timeout, \$killme_stdout2,
	qr/insert-tuple-lock-waiting/m);
$killme_stdout2 = '';
$killme_stderr2 = '';

# Kill with SIGKILL
$ret = PostgreSQL::Test::Utils::system_log('pg_ctl', 'kill', 'KILL', $pid);
is($ret, 0, 'killed process with KILL');

# Close that psql session
$killme->finish;

# Wait till the other session reports failure, ensuring that the postmaster
# has noticed its dead child and begun a restart cycle.
$killme_stdin2 .= qq[
SELECT pg_sleep($PostgreSQL::Test::Utils::timeout_default);
];
ok( pump_until(
		$killme2,
		$psql_timeout,
		\$killme_stderr2,
		qr/WARNING:  terminating connection because of crash of another server process|server closed the connection unexpectedly|connection to server was lost|could not send data to server/m
	),
	"second psql session died successfully after SIGKILL");
$killme2->finish;

# Wait till server finishes restarting
$node->poll_query_until('postgres', undef, '');

# Check for temporary files -- should be there
is( $node->safe_psql(
		'postgres', 'SELECT COUNT(1) FROM pg_ls_dir($$base/pgsql_tmp$$)'),
	qq(1),
	'one temporary file');

# Restart should remove the temporary files
$node->restart();

# Check the temporary files -- should be gone
is( $node->safe_psql(
		'postgres', 'SELECT COUNT(1) FROM pg_ls_dir($$base/pgsql_tmp$$)'),
	qq(0),
	'temporary file was removed');

$node->stop();

done_testing();
