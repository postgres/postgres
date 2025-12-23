# Checks waiting for the LSN replay on standby using
# the WAIT FOR command.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

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

# 3. Check that waiting for unreachable LSN triggers the timeout.  The
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

# 4. Check that WAIT FOR triggers an error if called on primary,
# within another function, or inside a transaction with an isolation level
# higher than READ COMMITTED.

$node_primary->psql('postgres', "WAIT FOR LSN '${lsn3}';",
	stderr => \$stderr);
ok( $stderr =~ /recovery is not in progress/,
	"get an error when running on the primary");

$node_standby->psql(
	'postgres',
	"BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; WAIT FOR LSN '${lsn3}';",
	stderr => \$stderr);
ok( $stderr =~
	  /WAIT FOR must be only called without an active or registered snapshot/,
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
	  /WAIT FOR must be only called without an active or registered snapshot/,
	"get an error when running within another function");

# 5. Check parameter validation error cases on standby before promotion
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

# 6. Check the scenario of multiple LSN waiters.  We make 5 background
# psql sessions each waiting for a corresponding insertion.  When waiting is
# finished, stored procedures logs if there are visible as many rows as
# should be.
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

ok(1, 'multiple LSN waiters reported consistent data');

# 7. Check that the standby promotion terminates the wait on LSN.  Start
# waiting for an unreachable LSN then promote.  Check the log for the relevant
# error message.  Also, check that waiting for already replayed LSN doesn't
# cause an error even after promotion.
my $lsn4 =
  $node_primary->safe_psql('postgres',
	"SELECT pg_current_wal_insert_lsn() + 10000000000");
my $lsn5 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
my $psql_session = $node_standby->background_psql('postgres');
$psql_session->query_until(
	qr/start/, qq[
	\\echo start
	WAIT FOR LSN '${lsn4}';
]);

# Make sure standby will be promoted at least at the primary insert LSN we
# have just observed.  Use pg_switch_wal() to force the insert LSN to be
# written then wait for standby to catchup.
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal();');
$node_primary->wait_for_catchup($node_standby);

$log_offset = -s $node_standby->logfile;
$node_standby->promote;
$node_standby->wait_for_log('recovery is not in progress', $log_offset);

ok(1, 'got error after standby promote');

$node_standby->safe_psql('postgres', "WAIT FOR LSN '${lsn5}';");

ok(1, 'wait for already replayed LSN exits immediately even after promotion');

$output = $node_standby->safe_psql(
	'postgres', qq[
	WAIT FOR LSN '${lsn4}' WITH (timeout '10ms', no_throw);]);
ok($output eq "not in recovery",
	"WAIT FOR returns correct status after standby promotion");


$node_standby->stop;
$node_primary->stop;

# If we send \q with $psql_session->quit the command can be sent to the session
# already closed. So \q is in initial script, here we only finish IPC::Run.
$psql_session->{run}->finish;

done_testing();
