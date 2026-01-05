# Checks waiting for the LSN using the WAIT FOR command.
# Tests standby modes (standby_replay/standby_write/standby_flush) on standby
# and primary_flush mode on primary.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Helper functions to control walreceiver for testing wait conditions.
# These allow us to stop WAL streaming so waiters block, then resume it.
my $saved_primary_conninfo;

sub stop_walreceiver
{
	my ($node) = @_;
	$saved_primary_conninfo = $node->safe_psql(
		'postgres', qq[
		SELECT pg_catalog.quote_literal(setting)
		FROM pg_settings
		WHERE name = 'primary_conninfo';
	]);
	$node->safe_psql(
		'postgres', qq[
		ALTER SYSTEM SET primary_conninfo = '';
		SELECT pg_reload_conf();
	]);

	$node->poll_query_until('postgres',
		"SELECT NOT EXISTS (SELECT * FROM pg_stat_wal_receiver);");
}

sub resume_walreceiver
{
	my ($node) = @_;
	$node->safe_psql(
		'postgres', qq[
		ALTER SYSTEM SET primary_conninfo = $saved_primary_conninfo;
		SELECT pg_reload_conf();
	]);

	$node->poll_query_until('postgres',
		"SELECT EXISTS (SELECT * FROM pg_stat_wal_receiver);");
}

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

# And some content and take a backup
$node_primary->safe_psql('postgres',
	"CREATE TABLE wait_test AS SELECT generate_series(1,10) AS a");
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Create a streaming standby with a 1 second delay from the backup
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
my $delay = 1;
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->append_conf(
	'postgresql.conf', qq[
	recovery_min_apply_delay = '${delay}s'
]);
$node_standby->start;

# 1. Make sure that WAIT FOR works: add new content to
# primary and memorize primary's insert LSN, then wait for that LSN to be
# replayed on standby.
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(11, 20))");
my $lsn1 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
my $output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn1}' WITH (timeout '1d');
	SELECT pg_lsn_cmp(pg_last_wal_replay_lsn(), '${lsn1}'::pg_lsn);
]);

# Make sure the current LSN on standby is at least as big as the LSN we
# observed on primary's before.
ok((split("\n", $output))[-1] >= 0,
	"standby reached the same LSN as primary after WAIT FOR");

# 2. Check that new data is visible after calling WAIT FOR
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(21, 30))");
my $lsn2 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn2}';
	SELECT count(*) FROM wait_test;
]);

# Make sure the count(*) on standby reflects the recent changes on primary
ok((split("\n", $output))[-1] eq 30,
	"standby reached the same LSN as primary");

# 3. Check that WAIT FOR works with standby_write, standby_flush, and
# primary_flush modes.
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(31, 40))");
my $lsn_write =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn_write}' WITH (MODE 'standby_write', timeout '1d');
	SELECT pg_lsn_cmp((SELECT written_lsn FROM pg_stat_wal_receiver), '${lsn_write}'::pg_lsn);
]);

ok( (split("\n", $output))[-1] >= 0,
	"standby wrote WAL up to target LSN after WAIT FOR with MODE 'standby_write'"
);

$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(41, 50))");
my $lsn_flush =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn_flush}' WITH (MODE 'standby_flush', timeout '1d');
	SELECT pg_lsn_cmp(pg_last_wal_receive_lsn(), '${lsn_flush}'::pg_lsn);
]);

ok( (split("\n", $output))[-1] >= 0,
	"standby flushed WAL up to target LSN after WAIT FOR with MODE 'standby_flush'"
);

# Check primary_flush mode on primary
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(51, 60))");
my $lsn_primary_flush =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$output = $node_primary->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn_primary_flush}' WITH (MODE 'primary_flush', timeout '1d');
	SELECT pg_lsn_cmp(pg_current_wal_flush_lsn(), '${lsn_primary_flush}'::pg_lsn);
]);

ok( (split("\n", $output))[-1] >= 0,
	"primary flushed WAL up to target LSN after WAIT FOR with MODE 'primary_flush'"
);

# 4. Check that waiting for unreachable LSN triggers the timeout.  The
# unreachable LSN must be well in advance.  So WAL records issued by
# the concurrent autovacuum could not affect that.
my $lsn3 =
  $node_primary->safe_psql('postgres',
	"SELECT pg_current_wal_insert_lsn() + 10000000000");
my $stderr;
$node_standby->safe_psql('postgres',
	"WAIT FOR LSN '${lsn2}' WITH (timeout '10ms');");
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${lsn3}' WITH (timeout '1000ms');",
	stderr => \$stderr);
ok( $stderr =~ /timed out while waiting for target LSN/,
	"get timeout on waiting for unreachable LSN");

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn2}' WITH (timeout '0.1s', no_throw);]);
ok($output eq "success",
	"WAIT FOR returns correct status after successful waiting");
$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn3}' WITH (timeout '10ms', no_throw);]);
ok($output eq "timeout", "WAIT FOR returns correct status after timeout");

# 5. Check mode validation: standby modes error on primary, primary mode errors
# on standby, and primary_flush works on primary.  Also check that WAIT FOR
# triggers an error if called within another function or inside a transaction
# with an isolation level higher than READ COMMITTED.

# Test standby_flush on primary - should error
$node_primary->psql(
	'postgres',
	"WAIT FOR LSN '${lsn3}' WITH (MODE 'standby_flush');",
	stderr => \$stderr);
ok($stderr =~ /recovery is not in progress/,
	"get an error when running standby_flush on the primary");

# Test primary_flush on standby - should error
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${lsn3}' WITH (MODE 'primary_flush');",
	stderr => \$stderr);
ok($stderr =~ /recovery is in progress/,
	"get an error when running primary_flush on the standby");

$node_standby->psql(
	'postgres',
	"BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; WAIT FOR LSN '${lsn3}';",
	stderr => \$stderr);
ok( $stderr =~
	  /WAIT FOR must be called without an active or registered snapshot/,
	"get an error when running in a transaction with an isolation level higher than REPEATABLE READ"
);

$node_primary->safe_psql(
	'postgres', qq[
CREATE FUNCTION pg_wal_replay_wait_wrap(target_lsn pg_lsn) RETURNS void AS \$\$
  BEGIN
    EXECUTE format('WAIT FOR LSN %L;', target_lsn);
  END
\$\$
LANGUAGE plpgsql;
]);

$node_primary->wait_for_catchup($node_standby);
$node_standby->psql(
	'postgres',
	"SELECT pg_wal_replay_wait_wrap('${lsn3}');",
	stderr => \$stderr);
ok( $stderr =~
	  /WAIT FOR must be called without an active or registered snapshot/,
	"get an error when running within another function");

# 6. Check parameter validation error cases on standby before promotion
my $test_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");

# Test negative timeout
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${test_lsn}' WITH (timeout '-1000ms');",
	stderr => \$stderr);
ok($stderr =~ /timeout cannot be negative/, "get error for negative timeout");

# Test unknown parameter with WITH clause
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${test_lsn}' WITH (unknown_param 'value');",
	stderr => \$stderr);
ok($stderr =~ /option "unknown_param" not recognized/,
	"get error for unknown parameter");

# Test duplicate TIMEOUT parameter with WITH clause
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${test_lsn}' WITH (timeout '1000', timeout '2000');",
	stderr => \$stderr);
ok( $stderr =~ /conflicting or redundant options/,
	"get error for duplicate TIMEOUT parameter");

# Test duplicate NO_THROW parameter with WITH clause
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${test_lsn}' WITH (no_throw, no_throw);",
	stderr => \$stderr);
ok( $stderr =~ /conflicting or redundant options/,
	"get error for duplicate NO_THROW parameter");

# Test syntax error - options without WITH keyword
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${test_lsn}' (timeout '100ms');",
	stderr => \$stderr);
ok($stderr =~ /syntax error/,
	"get syntax error when options specified without WITH keyword");

# Test syntax error - missing LSN
$node_standby->psql('postgres', "WAIT FOR TIMEOUT 1000;", stderr => \$stderr);
ok($stderr =~ /syntax error/, "get syntax error for missing LSN");

# Test invalid LSN format
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN 'invalid_lsn';",
	stderr => \$stderr);
ok($stderr =~ /invalid input syntax for type pg_lsn/,
	"get error for invalid LSN format");

# Test invalid timeout format
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${test_lsn}' WITH (timeout 'invalid');",
	stderr => \$stderr);
ok($stderr =~ /invalid timeout value/,
	"get error for invalid timeout format");

# Test new WITH clause syntax
$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn2}' WITH (timeout '0.1s', no_throw);]);
ok($output eq "success", "WAIT FOR WITH clause syntax works correctly");

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn3}' WITH (timeout 100, no_throw);]);
ok($output eq "timeout",
	"WAIT FOR WITH clause returns correct timeout status");

# Test WITH clause error case - invalid option
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${test_lsn}' WITH (invalid_option 'value');",
	stderr => \$stderr);
ok( $stderr =~ /option "invalid_option" not recognized/,
	"get error for invalid WITH clause option");

# Test invalid MODE value
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${test_lsn}' WITH (MODE 'invalid');",
	stderr => \$stderr);
ok($stderr =~ /unrecognized value for WAIT option "mode": "invalid"/,
	"get error for invalid MODE value");

# Test duplicate MODE parameter
$node_standby->psql(
	'postgres',
	"WAIT FOR LSN '${test_lsn}' WITH (MODE 'standby_replay', MODE 'standby_write');",
	stderr => \$stderr);
ok( $stderr =~ /conflicting or redundant options/,
	"get error for duplicate MODE parameter");

# 7a. Check the scenario of multiple standby_replay waiters.  We make 5
# background psql sessions each waiting for a corresponding insertion.  When
# waiting is finished, stored procedures logs if there are visible as many
# rows as should be.
$node_primary->safe_psql(
	'postgres', qq[
CREATE FUNCTION log_count(i int) RETURNS void AS \$\$
  DECLARE
    count int;
  BEGIN
    SELECT count(*) FROM wait_test INTO count;
    IF count >= 31 + i THEN
      RAISE LOG 'count %', i;
    END IF;
  END
\$\$
LANGUAGE plpgsql;

CREATE FUNCTION log_wait_done(prefix text, i int) RETURNS void AS \$\$
  BEGIN
    RAISE LOG '% %', prefix, i;
  END
\$\$
LANGUAGE plpgsql;
]);

$node_standby->safe_psql('postgres', "SELECT pg_wal_replay_pause();");

my @psql_sessions;
for (my $i = 0; $i < 5; $i++)
{
	$node_primary->safe_psql('postgres',
		"INSERT INTO wait_test VALUES (${i});");
	my $lsn =
	  $node_primary->safe_psql('postgres',
		"SELECT pg_current_wal_insert_lsn()");
	$psql_sessions[$i] = $node_standby->background_psql('postgres');
	$psql_sessions[$i]->query_until(
		qr/start/, qq[
		\\echo start
		WAIT FOR LSN '${lsn}';
		SELECT log_count(${i});
	]);
}

my $log_offset = -s $node_standby->logfile;
$node_standby->safe_psql('postgres', "SELECT pg_wal_replay_resume();");
for (my $i = 0; $i < 5; $i++)
{
	$node_standby->wait_for_log("count ${i}", $log_offset);
	$psql_sessions[$i]->quit;
}

ok(1, 'multiple standby_replay waiters reported consistent data');

# 7b. Check the scenario of multiple standby_write waiters.
# Stop walreceiver to ensure waiters actually block.
stop_walreceiver($node_standby);

# Generate WAL on primary (standby won't receive it yet)
my @write_lsns;
for (my $i = 0; $i < 5; $i++)
{
	$node_primary->safe_psql('postgres',
		"INSERT INTO wait_test VALUES (100 + ${i});");
	$write_lsns[$i] =
	  $node_primary->safe_psql('postgres',
		"SELECT pg_current_wal_insert_lsn()");
}

# Start standby_write waiters (they will block since walreceiver is stopped)
my @write_sessions;
for (my $i = 0; $i < 5; $i++)
{
	$write_sessions[$i] = $node_standby->background_psql('postgres');
	$write_sessions[$i]->query_until(
		qr/start/, qq[
		\\echo start
		WAIT FOR LSN '$write_lsns[$i]' WITH (MODE 'standby_write', timeout '1d');
		SELECT log_wait_done('write_done', $i);
	]);
}

# Verify waiters are blocked
$node_standby->poll_query_until('postgres',
	"SELECT count(*) = 5 FROM pg_stat_activity WHERE wait_event = 'WaitForWalWrite'"
);

# Restore walreceiver to unblock waiters
my $write_log_offset = -s $node_standby->logfile;
resume_walreceiver($node_standby);

# Wait for all waiters to complete and close sessions
for (my $i = 0; $i < 5; $i++)
{
	$node_standby->wait_for_log("write_done $i", $write_log_offset);
	$write_sessions[$i]->quit;
}

# Verify on standby that WAL was written up to the target LSN
$output = $node_standby->safe_psql('postgres',
	"SELECT pg_lsn_cmp((SELECT written_lsn FROM pg_stat_wal_receiver), '$write_lsns[4]'::pg_lsn);"
);

ok($output >= 0,
	"multiple standby_write waiters: standby wrote WAL up to target LSN");

# 7c. Check the scenario of multiple standby_flush waiters.
# Stop walreceiver to ensure waiters actually block.
stop_walreceiver($node_standby);

# Generate WAL on primary (standby won't receive it yet)
my @flush_lsns;
for (my $i = 0; $i < 5; $i++)
{
	$node_primary->safe_psql('postgres',
		"INSERT INTO wait_test VALUES (200 + ${i});");
	$flush_lsns[$i] =
	  $node_primary->safe_psql('postgres',
		"SELECT pg_current_wal_insert_lsn()");
}

# Start standby_flush waiters (they will block since walreceiver is stopped)
my @flush_sessions;
for (my $i = 0; $i < 5; $i++)
{
	$flush_sessions[$i] = $node_standby->background_psql('postgres');
	$flush_sessions[$i]->query_until(
		qr/start/, qq[
		\\echo start
		WAIT FOR LSN '$flush_lsns[$i]' WITH (MODE 'standby_flush', timeout '1d');
		SELECT log_wait_done('flush_done', $i);
	]);
}

# Verify waiters are blocked
$node_standby->poll_query_until('postgres',
	"SELECT count(*) = 5 FROM pg_stat_activity WHERE wait_event = 'WaitForWalFlush'"
);

# Restore walreceiver to unblock waiters
my $flush_log_offset = -s $node_standby->logfile;
resume_walreceiver($node_standby);

# Wait for all waiters to complete and close sessions
for (my $i = 0; $i < 5; $i++)
{
	$node_standby->wait_for_log("flush_done $i", $flush_log_offset);
	$flush_sessions[$i]->quit;
}

# Verify on standby that WAL was flushed up to the target LSN
$output = $node_standby->safe_psql('postgres',
	"SELECT pg_lsn_cmp(pg_last_wal_receive_lsn(), '$flush_lsns[4]'::pg_lsn);"
);

ok($output >= 0,
	"multiple standby_flush waiters: standby flushed WAL up to target LSN");

# 7d. Check the scenario of mixed standby mode waiters (standby_replay,
# standby_write, standby_flush) running concurrently.  We start 6 sessions:
# 2 for each mode, all waiting for the same target LSN.  We stop the
# walreceiver and pause replay to ensure all waiters block.  Then we resume
# replay and restart the walreceiver to verify they unblock and complete
# correctly.

# Stop walreceiver first to ensure we can control the flow without hanging
# (stopping it after pausing replay can hang if the startup process is paused).
stop_walreceiver($node_standby);

# Pause replay
$node_standby->safe_psql('postgres', "SELECT pg_wal_replay_pause();");

# Generate WAL on primary
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(301, 310));");
my $mixed_target_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");

# Start 6 waiters: 2 for each mode
my @mixed_sessions;
my @mixed_modes = ('standby_replay', 'standby_write', 'standby_flush');
for (my $i = 0; $i < 6; $i++)
{
	$mixed_sessions[$i] = $node_standby->background_psql('postgres');
	$mixed_sessions[$i]->query_until(
		qr/start/, qq[
		\\echo start
		WAIT FOR LSN '${mixed_target_lsn}' WITH (MODE '$mixed_modes[$i % 3]', timeout '1d');
		SELECT log_wait_done('mixed_done', $i);
	]);
}

# Verify all waiters are blocked
$node_standby->poll_query_until('postgres',
	"SELECT count(*) = 6 FROM pg_stat_activity WHERE wait_event LIKE 'WaitForWal%'"
);

# Resume replay (waiters should still be blocked as no WAL has arrived)
my $mixed_log_offset = -s $node_standby->logfile;
$node_standby->safe_psql('postgres', "SELECT pg_wal_replay_resume();");
$node_standby->poll_query_until('postgres',
	"SELECT NOT pg_is_wal_replay_paused();");

# Restore walreceiver to allow WAL to arrive
resume_walreceiver($node_standby);

# Wait for all sessions to complete and close them
for (my $i = 0; $i < 6; $i++)
{
	$node_standby->wait_for_log("mixed_done $i", $mixed_log_offset);
	$mixed_sessions[$i]->quit;
}

# Verify all modes reached the target LSN
$output = $node_standby->safe_psql(
	'postgres', qq[
	SELECT pg_lsn_cmp((SELECT written_lsn FROM pg_stat_wal_receiver), '${mixed_target_lsn}'::pg_lsn) >= 0 AND
	       pg_lsn_cmp(pg_last_wal_receive_lsn(), '${mixed_target_lsn}'::pg_lsn) >= 0 AND
	       pg_lsn_cmp(pg_last_wal_replay_lsn(), '${mixed_target_lsn}'::pg_lsn) >= 0;
]);

ok($output eq 't',
	"mixed mode waiters: all modes completed and reached target LSN");

# 7e. Check the scenario of multiple primary_flush waiters on primary.
# We start 5 background sessions waiting for different LSNs with primary_flush
# mode.  Each waiter logs when done.
my @primary_flush_lsns;
for (my $i = 0; $i < 5; $i++)
{
	$node_primary->safe_psql('postgres',
		"INSERT INTO wait_test VALUES (400 + ${i});");
	$primary_flush_lsns[$i] =
	  $node_primary->safe_psql('postgres',
		"SELECT pg_current_wal_insert_lsn()");
}

my $primary_flush_log_offset = -s $node_primary->logfile;

# Start primary_flush waiters
my @primary_flush_sessions;
for (my $i = 0; $i < 5; $i++)
{
	$primary_flush_sessions[$i] = $node_primary->background_psql('postgres');
	$primary_flush_sessions[$i]->query_until(
		qr/start/, qq[
		\\echo start
		WAIT FOR LSN '$primary_flush_lsns[$i]' WITH (MODE 'primary_flush', timeout '1d');
		SELECT log_wait_done('primary_flush_done', $i);
	]);
}

# The WAL should already be flushed, so waiters should complete quickly
for (my $i = 0; $i < 5; $i++)
{
	$node_primary->wait_for_log("primary_flush_done $i",
		$primary_flush_log_offset);
	$primary_flush_sessions[$i]->quit;
}

# Verify on primary that WAL was flushed up to the target LSN
$output = $node_primary->safe_psql('postgres',
	"SELECT pg_lsn_cmp(pg_current_wal_flush_lsn(), '$primary_flush_lsns[4]'::pg_lsn);"
);

ok($output >= 0,
	"multiple primary_flush waiters: primary flushed WAL up to target LSN");

# 8. Check that the standby promotion terminates all standby wait modes.  Start
# waiting for unreachable LSNs with standby_replay, standby_write, and
# standby_flush modes, then promote.  Check the log for the relevant error
# messages.  Also, check that waiting for already replayed LSN doesn't cause
# an error even after promotion.
my $lsn4 =
  $node_primary->safe_psql('postgres',
	"SELECT pg_current_wal_insert_lsn() + 10000000000");

my $lsn5 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");

# Start background sessions waiting for unreachable LSN with all modes
my @wait_modes = ('standby_replay', 'standby_write', 'standby_flush');
my @wait_sessions;
for (my $i = 0; $i < 3; $i++)
{
	$wait_sessions[$i] = $node_standby->background_psql('postgres');
	$wait_sessions[$i]->query_until(
		qr/start/, qq[
		\\echo start
		WAIT FOR LSN '${lsn4}' WITH (MODE '$wait_modes[$i]');
	]);
}

# Make sure standby will be promoted at least at the primary insert LSN we
# have just observed.  Use pg_switch_wal() to force the insert LSN to be
# written then wait for standby to catchup.
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal();');
$node_primary->wait_for_catchup($node_standby);

$log_offset = -s $node_standby->logfile;
$node_standby->promote;

# Wait for all three sessions to get the error (each mode has distinct message)
$node_standby->wait_for_log(qr/Recovery ended before target LSN.*was written/,
	$log_offset);
$node_standby->wait_for_log(qr/Recovery ended before target LSN.*was flushed/,
	$log_offset);
$node_standby->wait_for_log(
	qr/Recovery ended before target LSN.*was replayed/, $log_offset);

ok(1, 'promotion interrupted all wait modes');

$node_standby->safe_psql('postgres', "WAIT FOR LSN '${lsn5}';");

ok(1, 'wait for already replayed LSN exits immediately even after promotion');

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn4}' WITH (timeout '10ms', no_throw);]);
ok($output eq "not in recovery",
	"WAIT FOR returns correct status after standby promotion");


$node_standby->stop;
$node_primary->stop;

# If we send \q with $session->quit the command can be sent to the session
# already closed. So \q is in initial script, here we only finish IPC::Run.
for (my $i = 0; $i < 3; $i++)
{
	$wait_sessions[$i]->{run}->finish;
}

done_testing();
