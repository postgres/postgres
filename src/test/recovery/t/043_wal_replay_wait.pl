# Checks waiting for the lsn replay on standby using
# pg_wal_replay_wait() procedure.
use strict;
use warnings;

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
my $node_standby1 = PostgreSQL::Test::Cluster->new('standby');
my $delay = 1;
$node_standby1->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby1->append_conf(
	'postgresql.conf', qq[
	recovery_min_apply_delay = '${delay}s'
]);
$node_standby1->start;

# 1. Make sure that pg_wal_replay_wait() works: add new content to
# primary and memorize primary's insert LSN, then wait for that LSN to be
# replayed on standby.
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(11, 20))");
my $lsn1 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
my $output = $node_standby1->safe_psql(
	'postgres', qq[
	CALL pg_wal_replay_wait('${lsn1}', 1000000);
	SELECT pg_lsn_cmp(pg_last_wal_replay_lsn(), '${lsn1}'::pg_lsn);
]);

# Make sure the current LSN on standby is at least as big as the LSN we
# observed on primary's before.
ok($output >= 0,
	"standby reached the same LSN as primary after pg_wal_replay_wait()");

# 2. Check that new data is visible after calling pg_wal_replay_wait()
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(21, 30))");
my $lsn2 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$output = $node_standby1->safe_psql(
	'postgres', qq[
	CALL pg_wal_replay_wait('${lsn2}');
	SELECT count(*) FROM wait_test;
]);

# Make sure the count(*) on standby reflects the recent changes on primary
ok($output eq 30, "standby reached the same LSN as primary");

# 3. Check that waiting for unreachable LSN triggers the timeout.  The
# unreachable LSN must be well in advance.  So WAL records issued by
# the concurrent autovacuum could not affect that.
my $lsn3 =
  $node_primary->safe_psql('postgres',
	"SELECT pg_current_wal_insert_lsn() + 10000000000");
my $stderr;
$node_standby1->safe_psql('postgres',
	"CALL pg_wal_replay_wait('${lsn2}', 10);");
$node_standby1->psql(
	'postgres',
	"CALL pg_wal_replay_wait('${lsn3}', 1000);",
	stderr => \$stderr);
ok( $stderr =~ /timed out while waiting for target LSN/,
	"get timeout on waiting for unreachable LSN");

# 4. Check that pg_wal_replay_wait() triggers an error if called on primary,
# within another function, or inside a transaction with an isolation level
# higher than READ COMMITTED.

$node_primary->psql(
	'postgres',
	"CALL pg_wal_replay_wait('${lsn3}');",
	stderr => \$stderr);
ok( $stderr =~ /recovery is not in progress/,
	"get an error when running on the primary");

$node_standby1->psql(
	'postgres',
	"BEGIN ISOLATION LEVEL REPEATABLE READ; CALL pg_wal_replay_wait('${lsn3}');",
	stderr => \$stderr);
ok( $stderr =~
	  /pg_wal_replay_wait\(\) must be only called without an active or registered snapshot/,
	"get an error when running in a transaction with an isolation level higher than REPEATABLE READ"
);

$node_primary->safe_psql(
	'postgres', qq[
CREATE FUNCTION pg_wal_replay_wait_wrap(target_lsn pg_lsn) RETURNS void AS \$\$
  BEGIN
    CALL pg_wal_replay_wait(target_lsn);
  END
\$\$
LANGUAGE plpgsql;
]);

$node_primary->wait_for_catchup($node_standby1);
$node_standby1->psql(
	'postgres',
	"SELECT pg_wal_replay_wait_wrap('${lsn3}');",
	stderr => \$stderr);
ok( $stderr =~
	  /pg_wal_replay_wait\(\) must be only called without an active or registered snapshot/,
	"get an error when running within another function");

# 5. Also, check the scenario of multiple LSN waiters.  We make 5 background
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
$node_standby1->safe_psql('postgres', "SELECT pg_wal_replay_pause();");
my @psql_sessions;
for (my $i = 0; $i < 5; $i++)
{
	print($i);
	$node_primary->safe_psql('postgres',
		"INSERT INTO wait_test VALUES (${i});");
	my $lsn =
	  $node_primary->safe_psql('postgres',
		"SELECT pg_current_wal_insert_lsn()");
	$psql_sessions[$i] = $node_standby1->background_psql('postgres');
	$psql_sessions[$i]->query_until(
		qr/start/, qq[
		\\echo start
		CALL pg_wal_replay_wait('${lsn}');
		SELECT log_count(${i});
	]);
}
my $log_offset = -s $node_standby1->logfile;
$node_standby1->safe_psql('postgres', "SELECT pg_wal_replay_resume();");
for (my $i = 0; $i < 5; $i++)
{
	$node_standby1->wait_for_log("count ${i}", $log_offset);
	$psql_sessions[$i]->quit;
}

ok(1, 'multiple LSN waiters reported consistent data');

# 6. Check that the standby promotion terminates the wait on LSN.  Start
# waiting for an unreachable LSN then promote.  Check the log for the relevant
# error message.  Also, check that waiting for already replayed LSN doesn't
# cause an error even after promotion.
my $lsn4 =
  $node_primary->safe_psql('postgres',
	"SELECT pg_current_wal_insert_lsn() + 10000000000");
my $lsn5 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
my $psql_session = $node_standby1->background_psql('postgres');
$psql_session->query_until(
	qr/start/, qq[
	\\echo start
	CALL pg_wal_replay_wait('${lsn4}');
]);

$log_offset = -s $node_standby1->logfile;
$node_standby1->promote;
$node_standby1->wait_for_log('recovery is not in progress', $log_offset);

ok(1, 'got error after standby promote');

$node_standby1->safe_psql('postgres', "CALL pg_wal_replay_wait('${lsn5}');");

ok(1,
	'wait for already replayed LSN exists immediately even after promotion');

$node_standby1->stop;
$node_primary->stop;

# If we send \q with $psql_session->quit the command can be sent to the session
# already closed. So \q is in initial script, here we only finish IPC::Run.
$psql_session->{run}->finish;

done_testing();
