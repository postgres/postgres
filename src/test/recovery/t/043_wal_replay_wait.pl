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
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
my $delay = 1;
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->append_conf(
	'postgresql.conf', qq[
	recovery_min_apply_delay = '${delay}s'
]);
$node_standby->start;


# Make sure that pg_wal_replay_wait() works: add new content to
# primary and memorize primary's insert LSN, then wait for that LSN to be
# replayed on standby.
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(11, 20))");
my $lsn1 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
my $output = $node_standby->safe_psql(
	'postgres', qq[
	CALL pg_wal_replay_wait('${lsn1}', 1000000);
	SELECT pg_lsn_cmp(pg_last_wal_replay_lsn(), '${lsn1}'::pg_lsn);
]);

# Make sure the current LSN on standby is at least as big as the LSN we
# observed on primary's before.
ok($output >= 0,
	"standby reached the same LSN as primary after pg_wal_replay_wait()");

# Check that new data is visible after calling pg_wal_replay_wait()
$node_primary->safe_psql('postgres',
	"INSERT INTO wait_test VALUES (generate_series(21, 30))");
my $lsn2 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$output = $node_standby->safe_psql(
	'postgres', qq[
	CALL pg_wal_replay_wait('${lsn2}');
	SELECT count(*) FROM wait_test;
]);

# Make sure the current LSN on standby and is the same as primary's LSN
ok($output eq 30, "standby reached the same LSN as primary");

# Check that waiting for unreachable LSN triggers the timeout.  The
# unreachable LSN must be well in advance.  So WAL records issued by
# the concurrent autovacuum could not affect that.
my $lsn3 =
  $node_primary->safe_psql('postgres',
	"SELECT pg_current_wal_insert_lsn() + 10000000000");
my $stderr;
$node_standby->safe_psql('postgres',
	"CALL pg_wal_replay_wait('${lsn2}', 10);");
$node_standby->psql(
	'postgres',
	"CALL pg_wal_replay_wait('${lsn3}', 1000);",
	stderr => \$stderr);
ok( $stderr =~ /timed out while waiting for target LSN/,
	"get timeout on waiting for unreachable LSN");

# Check that the standby promotion terminates the wait on LSN.  Start
# waiting for unreachable LSN then promote.  Check the log for the relevant
# error message.
my $psql_session = $node_standby->background_psql('postgres');
$psql_session->query_until(
	qr/start/, qq[
	\\echo start
	CALL pg_wal_replay_wait('${lsn3}');
]);

my $log_offset = -s $node_standby->logfile;
$node_standby->promote;
$node_standby->wait_for_log('recovery is not in progress', $log_offset);

$node_standby->stop;
$node_primary->stop;
done_testing();
