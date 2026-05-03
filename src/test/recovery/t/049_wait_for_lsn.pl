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

# Stop the walreceiver on $node by clearing primary_conninfo and waiting
# until pg_stat_wal_receiver becomes empty.  Used to freeze the
# walreceiver-tracked positions (writtenUpto, flushedUpto) so a fencepost
# test can rely on them not advancing.  The previous value is saved for
# resume_walreceiver().
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

# Restart the walreceiver on $node by restoring primary_conninfo to the
# value captured by stop_walreceiver() and waiting until walreceiver
# reconnects.  Must be paired with a prior stop_walreceiver() call.
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

# Verify the wait predicate "target <= currentLSN" at the boundary.
# Given $current_lsn (the frozen position for $mode), check that:
#   target == current        -> success (predicate is <=)
#   target == current - 1    -> success
#   target == current + 1    -> timeout
# The caller must ensure that the relevant LSN position on $node is
# actually frozen (e.g. walreceiver stopped and replay paused), otherwise
# the "+1" case may racily succeed.  Returns ($lsn_minus, $lsn_plus) so
# the caller can reuse them, e.g. to drive an async wakeup test.
sub check_wait_for_lsn_fencepost
{
	my ($node, $mode, $current_lsn, $label) = @_;

	my $lsn_minus = $node->safe_psql('postgres',
		"SELECT ('$current_lsn'::pg_lsn - 1)::text");
	my $lsn_plus = $node->safe_psql('postgres',
		"SELECT ('$current_lsn'::pg_lsn + 1)::text");

	foreach my $case (
		[ $current_lsn, 'success', 'target == current succeeds', '5s' ],
		[ $lsn_minus, 'success', 'target == current - 1 succeeds', '5s' ],
		[ $lsn_plus, 'timeout', 'target == current + 1 times out', '500ms' ])
	{
		my ($target_lsn, $expected, $desc, $timeout) = @$case;
		my $output = $node->safe_psql(
			'postgres', qq[
			WAIT FOR LSN '${target_lsn}'
				WITH (MODE '$mode', timeout '$timeout', no_throw);]);

		is($output, $expected, "$label: $desc");
	}

	return ($lsn_minus, $lsn_plus);
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
# triggers an error if called within a function, procedure, anonymous DO block,
# or inside a transaction with an isolation level higher than READ COMMITTED.

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

# Test wrapping WAIT FOR into function, procedure, and anonymous DO block --
# should error
$node_primary->safe_psql(
	'postgres', qq[
CREATE FUNCTION pg_wal_replay_wait_wrap(target_lsn pg_lsn) RETURNS void AS \$\$
  BEGIN
    EXECUTE format('WAIT FOR LSN %L;', target_lsn);
  END
\$\$
LANGUAGE plpgsql;

CREATE PROCEDURE pg_wal_replay_wait_proc(target_lsn pg_lsn) AS \$\$
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
ok($stderr =~ /WAIT FOR can only be executed as a top-level statement/,
	"get an error when running within a function");

$node_standby->psql(
	'postgres',
	"CALL pg_wal_replay_wait_proc('${lsn3}');",
	stderr => \$stderr);
ok($stderr =~ /WAIT FOR can only be executed as a top-level statement/,
	"get an error when running within a procedure");

$node_standby->psql(
	'postgres',
	"DO \$\$ BEGIN EXECUTE format('WAIT FOR LSN %L;', '${lsn3}'); END \$\$;",
	stderr => \$stderr);
ok($stderr =~ /WAIT FOR can only be executed as a top-level statement/,
	"get an error when running within a DO block");

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

# 9. Archive-only standby tests: verify standby_write/standby_flush work
# without a walreceiver.  These exercises the replay-position floor in
# GetCurrentLSNForWaitType().
#
# We set up a separate primary with archiving and an archive-only standby
# (has_restoring, no has_streaming), so no walreceiver ever starts and the
# shared walreceiver positions (writtenUpto, flushedUpto) stay at their
# zero-initialized values.

my $arc_primary = PostgreSQL::Test::Cluster->new('arc_primary');
$arc_primary->init(has_archiving => 1, allows_streaming => 1);
$arc_primary->start;

$arc_primary->safe_psql('postgres',
	"CREATE TABLE arc_test AS SELECT generate_series(1,10) AS a");

my $arc_backup_name = 'arc_backup';
$arc_primary->backup($arc_backup_name);

# Generate WAL that will be archived and replayed on the standby.
$arc_primary->safe_psql('postgres',
	"INSERT INTO arc_test VALUES (generate_series(11, 20))");
my $arc_target_lsn =
  $arc_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");

# Force WAL to be archived by switching segments, then wait for archiving.
my $arc_segment = $arc_primary->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_current_wal_lsn())");
$arc_primary->safe_psql('postgres', "SELECT pg_switch_wal()");
$arc_primary->poll_query_until('postgres',
	qq{SELECT last_archived_wal >= '$arc_segment' FROM pg_stat_archiver}, 't')
  or die "Timed out waiting for WAL archiving on arc_primary";

# Create an archive-only standby: has_restoring but NOT has_streaming.
# No primary_conninfo means no walreceiver will start.
my $arc_standby = PostgreSQL::Test::Cluster->new('arc_standby');
$arc_standby->init_from_backup($arc_primary, $arc_backup_name,
	has_restoring => 1);
$arc_standby->start;

# Wait for the standby to replay past our target LSN via archive recovery.
$arc_standby->poll_query_until('postgres',
	qq{SELECT pg_wal_lsn_diff(pg_last_wal_replay_lsn(), '$arc_target_lsn') >= 0}
) or die "Timed out waiting for archive replay on arc_standby";

# Sanity: verify no walreceiver is running.
$output = $arc_standby->safe_psql('postgres',
	"SELECT count(*) FROM pg_stat_wal_receiver");
is($output, '0', "arc_standby has no walreceiver");

# 9a. Getter fallback: standby_write/standby_flush succeed immediately when
# the target LSN has already been replayed, even though writtenUpto and
# flushedUpto are zero.  GetCurrentLSNForWaitType() returns
# Max(walrcv_pos, replay), so replay >= target satisfies the check on the
# first loop iteration without ever sleeping.

$output = $arc_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${arc_target_lsn}'
		WITH (MODE 'standby_write', timeout '3s', no_throw);]);
ok($output eq "success",
	"standby_write succeeds on archive-only standby (getter fallback)");

$output = $arc_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${arc_target_lsn}'
		WITH (MODE 'standby_flush', timeout '3s', no_throw);]);
ok($output eq "success",
	"standby_flush succeeds on archive-only standby (getter fallback)");

# 9b. Replay waker: standby_write/standby_flush waiters that go to sleep
# (target > replay at entry) are woken when replay catches up.  This tests
# that PerformWalRecovery() calls WaitLSNWakeup for STANDBY_WRITE and
# STANDBY_FLUSH, not just STANDBY_REPLAY.
#
# Pause replay, archive more WAL, start background waiters, then resume
# replay and verify the waiters complete.

$arc_standby->safe_psql('postgres', "SELECT pg_wal_replay_pause()");

# Generate more WAL and archive it.
$arc_primary->safe_psql('postgres',
	"INSERT INTO arc_test VALUES (generate_series(21, 30))");
my $arc_target_lsn2 =
  $arc_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");

my $arc_segment2 = $arc_primary->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_current_wal_lsn())");
$arc_primary->safe_psql('postgres', "SELECT pg_switch_wal()");
$arc_primary->poll_query_until('postgres',
	qq{SELECT last_archived_wal >= '$arc_segment2' FROM pg_stat_archiver},
	't')
  or die "Timed out waiting for WAL archiving on arc_primary (round 2)";

# Start background waiters.  With replay paused, target > replay, so they
# will sleep on WaitLatch.  They can only be woken by the replay-loop
# WaitLSNWakeup calls.
my $arc_write_session = $arc_standby->background_psql('postgres');
$arc_write_session->query_until(
	qr/start/, qq[
	\\echo start
	WAIT FOR LSN '${arc_target_lsn2}'
		WITH (MODE 'standby_write', timeout '1d', no_throw);
]);

my $arc_flush_session = $arc_standby->background_psql('postgres');
$arc_flush_session->query_until(
	qr/start/, qq[
	\\echo start
	WAIT FOR LSN '${arc_target_lsn2}'
		WITH (MODE 'standby_flush', timeout '1d', no_throw);
]);

# Verify both waiters are blocked.
$arc_standby->poll_query_until('postgres',
	"SELECT count(*) = 2 FROM pg_stat_activity WHERE wait_event LIKE 'WaitForWal%'"
) or die "Timed out waiting for arc_standby waiters to block";

# Resume replay.  The startup process should wake the STANDBY_WRITE and
# STANDBY_FLUSH waiters as it replays past arc_target_lsn2.
$arc_standby->safe_psql('postgres', "SELECT pg_wal_replay_resume()");

$arc_write_session->quit;
$arc_flush_session->quit;
chomp($arc_write_session->{stdout});
chomp($arc_flush_session->{stdout});

is($arc_write_session->{stdout},
	'success',
	"standby_write waiter woken by replay on archive-only standby");
is($arc_flush_session->{stdout},
	'success',
	"standby_flush waiter woken by replay on archive-only standby");

$arc_standby->stop;
$arc_primary->stop;

# 10. Fresh-shmem walreceiver startup (29e7dbf5e4d).
# RequestXLogStreaming() initializes writtenUpto/flushedUpto to the
# segment-aligned receiveStart only when receiveStart was invalid.
# Restart the standby with the primary stopped, so the walreceiver cannot
# connect and advance these values past the initial one before we observe it.

my $rcv_primary = PostgreSQL::Test::Cluster->new('rcv_primary');
$rcv_primary->init(allows_streaming => 1);
# No background WAL during our probes.
$rcv_primary->append_conf('postgresql.conf', 'autovacuum = off');
$rcv_primary->start;
$rcv_primary->safe_psql('postgres',
	"CREATE TABLE rcv_test AS SELECT generate_series(1,10) AS a");

my $rcv_backup = 'rcv_backup';
$rcv_primary->backup($rcv_backup);

my $rcv_standby = PostgreSQL::Test::Cluster->new('rcv_standby');
$rcv_standby->init_from_backup($rcv_primary, $rcv_backup, has_streaming => 1);
$rcv_standby->start;

# Switch WAL segments mid-stream so the replay ends mid-segment after the
# upcoming standby restart.  That guarantees the initial value <
# final replay LSN.
$rcv_primary->safe_psql('postgres',
	"INSERT INTO rcv_test VALUES (generate_series(11, 100))");
$rcv_primary->safe_psql('postgres', "SELECT pg_switch_wal()");
$rcv_primary->safe_psql('postgres',
	"INSERT INTO rcv_test VALUES (generate_series(101, 110))");
$rcv_primary->wait_for_catchup($rcv_standby);

# Restart the standby with the primary down: WalRcvData is initialized, but
# the walreceiver cannot connect and update writtenUpto/flushedUpto.  So,
# the initial flushedUpto stays observable via pg_last_wal_receive_lsn().
$rcv_standby->stop;
$rcv_primary->stop;
$rcv_standby->start;

$rcv_standby->poll_query_until('postgres',
	"SELECT pg_last_wal_receive_lsn() IS NOT NULL;")
  or die "walreceiver initial value did not become visible";

# Freeze the replay so the (received, replay] window stays observable.
$rcv_standby->safe_psql('postgres', "SELECT pg_wal_replay_pause()");
$rcv_standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "Timed out waiting for rcv_standby replay to pause";

my $rcv_receive =
  $rcv_standby->safe_psql('postgres', "SELECT pg_last_wal_receive_lsn()");
my $rcv_replay =
  $rcv_standby->safe_psql('postgres', "SELECT pg_last_wal_replay_lsn()");
my $rcv_gap = $rcv_standby->safe_psql('postgres',
	"SELECT pg_wal_lsn_diff('$rcv_replay'::pg_lsn, '$rcv_receive'::pg_lsn) > 0"
);
ok($rcv_gap eq 't',
	"replay sits ahead of initial walreceiver flush position");

my $rcv_receive_offset = $rcv_standby->safe_psql(
	'postgres',
	"SELECT mod(pg_wal_lsn_diff('$rcv_receive'::pg_lsn, '0/0'::pg_lsn),
				setting::numeric)::int
	   FROM pg_settings
	  WHERE name = 'wal_segment_size'");
is($rcv_receive_offset, '0',
	"initial walreceiver flush position is segment-aligned");

# WAIT FOR an $rcv_replay LSN succeeds in standby_write / standby_flush
# modes thanks to GetCurrentLSNForWaitType() taking replay LSN as the floor.
# We observe flushedUpto directly via pg_last_wal_receive_lsn().  writtenUpto
# is covered indirectly: without the replay-position floor, standby_write would
# wait at the seeded segment-start position and time out.
foreach my $rcv_mode ('standby_write', 'standby_flush')
{
	$output = $rcv_standby->safe_psql(
		'postgres', qq[
		WAIT FOR LSN '${rcv_replay}'
			WITH (MODE '$rcv_mode', timeout '5s', no_throw);]);
	ok($output eq "success",
		"$rcv_mode succeeds for already-replayed LSN after standby restart");
}

# Restore primary and resume replay so section 11 can reuse the clusters.
# Generate fresh WAL after reconnecting so the walreceiver advances its
# flush position past the replay position before we freeze both frontiers.
$rcv_standby->safe_psql('postgres', "SELECT pg_wal_replay_resume()");
$rcv_primary->start;
$rcv_primary->safe_psql('postgres',
	"INSERT INTO rcv_test VALUES (generate_series(111, 120))");
$rcv_primary->wait_for_catchup($rcv_standby);

# 11. Off-by-one boundary checks for the wait predicate target <=
# currentLSN.  Stop the walreceiver before pausing replay (stopping
# after pause can hang -- see section 7d) so both replay and
# walreceiver positions are frozen.
stop_walreceiver($rcv_standby);
$rcv_standby->safe_psql('postgres', "SELECT pg_wal_replay_pause()");
$rcv_standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "Timed out waiting for rcv_standby replay to pause";

# 11a. standby_replay exact fencepost.  The replay position is frozen, so this
# probes the standby_replay predicate directly.
my $replay_lsn =
  $rcv_standby->safe_psql('postgres', "SELECT pg_last_wal_replay_lsn()");
my (undef, $replay_lsn_plus) =
  check_wait_for_lsn_fencepost($rcv_standby, 'standby_replay', $replay_lsn,
	'standby_replay');

# 11b. standby_flush exact fencepost.  pg_last_wal_receive_lsn() exposes the
# flushed walreceiver position even after walreceiver exits, so this probes
# the standby_flush predicate directly.  standby_write has no stable
# SQL-visible boundary once walreceiver is stopped; it is covered by the
# replay-floor and waiter wakeup tests above.
my $flush_lsn =
  $rcv_standby->safe_psql('postgres', "SELECT pg_last_wal_receive_lsn()");
my $flush_covers_replay = $rcv_standby->safe_psql('postgres',
	"SELECT pg_wal_lsn_diff('$flush_lsn'::pg_lsn, '$replay_lsn'::pg_lsn) >= 0"
);
ok($flush_covers_replay eq 't',
	"standby_flush boundary is not masked by replay floor");

check_wait_for_lsn_fencepost($rcv_standby, 'standby_flush', $flush_lsn,
	'standby_flush');

# 11c. A sleeping waiter at current + 1 wakes once replay advances
# past it.  Start the waiter while replay is still paused so it is
# guaranteed to sleep at replay_lsn_plus regardless of whether
# flush_lsn > replay_lsn.  Then resume replay and restart the
# walreceiver to deliver new WAL.
$rcv_primary->safe_psql('postgres',
	"INSERT INTO rcv_test VALUES (generate_series(200, 210))");

my $boundary_session = $rcv_standby->background_psql('postgres');
$boundary_session->query_until(
	qr/start/, qq[
	\\echo start
	WAIT FOR LSN '${replay_lsn_plus}'
		WITH (MODE 'standby_replay', timeout '1d', no_throw);
]);

$rcv_standby->poll_query_until('postgres',
	"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'WaitForWalReplay'"
) or die "Boundary waiter did not sleep";

$rcv_standby->safe_psql('postgres', "SELECT pg_wal_replay_resume()");
resume_walreceiver($rcv_standby);
$boundary_session->quit;
chomp($boundary_session->{stdout});
is($boundary_session->{stdout},
	'success',
	"standby_replay: waiter at current + 1 wakes when replay advances");

$rcv_standby->stop;
$rcv_primary->stop;

# 12. Timeline switch on a cascade standby.  A WAIT FOR LSN waiter on
# a cascade standby must survive its upstream's promotion: the
# cascade walreceiver reconnects on the new timeline and replay
# continues across the boundary.

my $tl_primary = PostgreSQL::Test::Cluster->new('tl_primary');
$tl_primary->init(allows_streaming => 1);
$tl_primary->append_conf('postgresql.conf', 'autovacuum = off');
$tl_primary->start;
$tl_primary->safe_psql('postgres',
	"CREATE TABLE tl_test AS SELECT generate_series(1, 10) AS a");

my $tl_backup = 'tl_backup';
$tl_primary->backup($tl_backup);

my $tl_standby1 = PostgreSQL::Test::Cluster->new('tl_standby1');
$tl_standby1->init_from_backup($tl_primary, $tl_backup, has_streaming => 1);
$tl_standby1->start;

# standby2 cascades from standby1.
my $tl_backup2 = 'tl_backup2';
$tl_standby1->backup($tl_backup2);

my $tl_standby2 = PostgreSQL::Test::Cluster->new('tl_standby2');
$tl_standby2->init_from_backup($tl_standby1, $tl_backup2, has_streaming => 1);
$tl_standby2->start;

$tl_primary->safe_psql('postgres',
	"INSERT INTO tl_test VALUES (generate_series(11, 20))");
$tl_primary->wait_for_catchup($tl_standby1);
$tl_standby1->wait_for_catchup($tl_standby2);

# Target LSN well past current insert LSN, so reaching it requires
# WAL produced on the new timeline.  Pause replay on standby2 to
# guarantee the waiter is asleep when the switch happens.
my $tl_target = $tl_primary->safe_psql('postgres',
	"SELECT (pg_current_wal_insert_lsn() + 65536)::text");

$tl_standby2->safe_psql('postgres', "SELECT pg_wal_replay_pause()");
$tl_standby2->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "Timed out waiting for tl_standby2 replay to pause";

my $tl_session = $tl_standby2->background_psql('postgres');
$tl_session->query_until(
	qr/start/, qq[
	\\echo start
	WAIT FOR LSN '${tl_target}'
		WITH (MODE 'standby_replay', timeout '1d', no_throw);
]);

$tl_standby2->poll_query_until('postgres',
	"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'WaitForWalReplay'"
) or die "Cascade waiter did not sleep before promotion";

# Promote standby1 to TLI 2; produce enough WAL on the new timeline
# to push past tl_target and force a segment switch.
$tl_standby1->promote;
$tl_standby1->safe_psql('postgres',
	"INSERT INTO tl_test VALUES (generate_series(21, 1020))");
$tl_standby1->safe_psql('postgres', "SELECT pg_switch_wal()");

$tl_standby2->safe_psql('postgres', "SELECT pg_wal_replay_resume()");

$tl_standby2->poll_query_until('postgres',
	"SELECT received_tli > 1 FROM pg_stat_wal_receiver")
  or die "tl_standby2 did not follow upstream timeline switch";

$tl_session->quit;
chomp($tl_session->{stdout});
is($tl_session->{stdout}, 'success',
	"WAIT FOR LSN survives upstream promotion and timeline switch on cascade standby"
);

$tl_standby2->stop;
$tl_standby1->stop;
$tl_primary->stop;

done_testing();
