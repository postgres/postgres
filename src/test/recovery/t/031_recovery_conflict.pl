# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test that connections to a hot standby are correctly canceled when a
# recovery conflict is detected Also, test that statistics in
# pg_stat_database_conflicts are populated correctly

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => "disabled due to instability";

# Set up nodes
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

my $tablespace1 = "test_recovery_conflict_tblspc";

$node_primary->append_conf(
	'postgresql.conf', qq[
allow_in_place_tablespaces = on
log_temp_files = 0

# for deadlock test
max_prepared_transactions = 10

# wait some to test the wait paths as well, but not long for obvious reasons
max_standby_streaming_delay = 50ms

temp_tablespaces = $tablespace1
# Some of the recovery conflict logging code only gets exercised after
# deadlock_timeout. The test doesn't rely on that additional output, but it's
# nice to get some minimal coverage of that code.
log_recovery_conflict_waits = on
deadlock_timeout = 10ms
]);
$node_primary->start;

my $backup_name = 'my_backup';

$node_primary->safe_psql('postgres',
	qq[CREATE TABLESPACE $tablespace1 LOCATION '']);

$node_primary->backup($backup_name);
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);

$node_standby->start;

my $test_db = "test_db";

# use a new database, to trigger database recovery conflict
$node_primary->safe_psql('postgres', "CREATE DATABASE $test_db");

# test schema / data
my $table1 = "test_recovery_conflict_table1";
my $table2 = "test_recovery_conflict_table2";
$node_primary->safe_psql(
	$test_db, qq[
CREATE TABLE ${table1}(a int, b int);
INSERT INTO $table1 SELECT i % 3, 0 FROM generate_series(1,20) i;
CREATE TABLE ${table2}(a int, b int);
]);
my $primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);


# a longrunning psql that we can use to trigger conflicts
my $psql_timeout = IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default);
my %psql_standby = ('stdin' => '', 'stdout' => '');
$psql_standby{run} =
  $node_standby->background_psql($test_db, \$psql_standby{stdin},
	\$psql_standby{stdout},
	$psql_timeout);
$psql_standby{stdout} = '';

my $expected_conflicts = 0;


## RECOVERY CONFLICT 1: Buffer pin conflict
my $sect = "buffer pin conflict";
$expected_conflicts++;

# Aborted INSERT on primary that will be cleaned up by vacuum. Has to be old
# enough so that there's not a snapshot conflict before the buffer pin
# conflict.

$node_primary->safe_psql(
	$test_db,
	qq[
	BEGIN;
	INSERT INTO $table1 VALUES (1,0);
	ROLLBACK;
	-- ensure flush, rollback doesn't do so
	BEGIN; LOCK $table1; COMMIT;
	]);

$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

my $cursor1 = "test_recovery_conflict_cursor";

# DECLARE and use a cursor on standby, causing buffer with the only block of
# the relation to be pinned on the standby
$psql_standby{stdin} .= qq[
        BEGIN;
        DECLARE $cursor1 CURSOR FOR SELECT b FROM $table1;
        FETCH FORWARD FROM $cursor1;
        ];
# FETCH FORWARD should have returned a 0 since all values of b in the table
# are 0
ok(pump_until_standby(qr/^0$/m),
	"$sect: cursor with conflicting pin established");

# to check the log starting now for recovery conflict messages
my $log_location = -s $node_standby->logfile;

# VACUUM on the primary
$node_primary->safe_psql($test_db, qq[VACUUM $table1;]);

# Wait for catchup. Existing connection will be terminated before replay is
# finished, so waiting for catchup ensures that there is no race between
# encountering the recovery conflict which causes the disconnect and checking
# the logfile for the terminated connection.
$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

check_conflict_log("User was holding shared buffer pin for too long");
reconnect_and_clear();
check_conflict_stat("bufferpin");


## RECOVERY CONFLICT 2: Snapshot conflict
$sect = "snapshot conflict";
$expected_conflicts++;

$node_primary->safe_psql($test_db,
	qq[INSERT INTO $table1 SELECT i, 0 FROM generate_series(1,20) i]);
$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

# DECLARE and FETCH from cursor on the standby
$psql_standby{stdin} .= qq[
        BEGIN;
        DECLARE $cursor1 CURSOR FOR SELECT b FROM $table1;
        FETCH FORWARD FROM $cursor1;
        ];
ok( pump_until(
		$psql_standby{run},     $psql_timeout,
		\$psql_standby{stdout}, qr/^0$/m,),
	"$sect: cursor with conflicting snapshot established");

# Do some HOT updates
$node_primary->safe_psql($test_db,
	qq[UPDATE $table1 SET a = a + 1 WHERE a > 2;]);

# VACUUM, pruning those dead tuples
$node_primary->safe_psql($test_db, qq[VACUUM $table1;]);

# Wait for attempted replay of PRUNE records
$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

check_conflict_log(
	"User query might have needed to see row versions that must be removed");
reconnect_and_clear();
check_conflict_stat("snapshot");


## RECOVERY CONFLICT 3: Lock conflict
$sect = "lock conflict";
$expected_conflicts++;

# acquire lock to conflict with
$psql_standby{stdin} .= qq[
        BEGIN;
        LOCK TABLE $table1 IN ACCESS SHARE MODE;
        SELECT 1;
        ];
ok(pump_until_standby(qr/^1$/m), "$sect: conflicting lock acquired");

# DROP TABLE containing block which standby has in a pinned buffer
$node_primary->safe_psql($test_db, qq[DROP TABLE $table1;]);

$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

check_conflict_log("User was holding a relation lock for too long");
reconnect_and_clear();
check_conflict_stat("lock");


## RECOVERY CONFLICT 4: Tablespace conflict
$sect = "tablespace conflict";
$expected_conflicts++;

# DECLARE a cursor for a query which, with sufficiently low work_mem, will
# spill tuples into temp files in the temporary tablespace created during
# setup.
$psql_standby{stdin} .= qq[
        BEGIN;
        SET work_mem = '64kB';
        DECLARE $cursor1 CURSOR FOR
          SELECT count(*) FROM generate_series(1,6000);
        FETCH FORWARD FROM $cursor1;
        ];
ok(pump_until_standby(qr/^6000$/m),
	"$sect: cursor with conflicting temp file established");

# Drop the tablespace currently containing spill files for the query on the
# standby
$node_primary->safe_psql($test_db, qq[DROP TABLESPACE $tablespace1;]);

$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

check_conflict_log(
	"User was or might have been using tablespace that must be dropped");
reconnect_and_clear();
check_conflict_stat("tablespace");


## RECOVERY CONFLICT 5: Deadlock
$sect = "startup deadlock";
$expected_conflicts++;

# Want to test recovery deadlock conflicts, not buffer pin conflicts. Without
# changing max_standby_streaming_delay it'd be timing dependent what we hit
# first
$node_standby->adjust_conf(
	'postgresql.conf',
	'max_standby_streaming_delay',
	"${PostgreSQL::Test::Utils::timeout_default}s");
$node_standby->restart();
reconnect_and_clear();

# Generate a few dead rows, to later be cleaned up by vacuum. Then acquire a
# lock on another relation in a prepared xact, so it's held continuously by
# the startup process. The standby psql will block acquiring that lock while
# holding a pin that vacuum needs, triggering the deadlock.
$node_primary->safe_psql(
	$test_db,
	qq[
CREATE TABLE $table1(a int, b int);
INSERT INTO $table1 VALUES (1);
BEGIN;
INSERT INTO $table1(a) SELECT generate_series(1, 100) i;
ROLLBACK;
BEGIN;
LOCK TABLE $table2;
PREPARE TRANSACTION 'lock';
INSERT INTO $table1(a) VALUES (170);
SELECT txid_current();
]);

$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

$psql_standby{stdin} .= qq[
    BEGIN;
    -- hold pin
    DECLARE $cursor1 CURSOR FOR SELECT a FROM $table1;
    FETCH FORWARD FROM $cursor1;
    -- wait for lock held by prepared transaction
	SELECT * FROM $table2;
    ];
ok( pump_until(
		$psql_standby{run},     $psql_timeout,
		\$psql_standby{stdout}, qr/^1$/m,),
	"$sect: cursor holding conflicting pin, also waiting for lock, established"
);

# just to make sure we're waiting for lock already
ok( $node_standby->poll_query_until(
		'postgres', qq[
SELECT 'waiting' FROM pg_locks WHERE locktype = 'relation' AND NOT granted;
], 'waiting'),
	"$sect: lock acquisition is waiting");

# VACUUM will prune away rows, causing a buffer pin conflict, while standby
# psql is waiting on lock
$node_primary->safe_psql($test_db, qq[VACUUM $table1;]);
$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

check_conflict_log("User transaction caused buffer deadlock with recovery.");
reconnect_and_clear();
check_conflict_stat("deadlock");

# clean up for next tests
$node_primary->safe_psql($test_db, qq[ROLLBACK PREPARED 'lock';]);
$node_standby->adjust_conf('postgresql.conf', 'max_standby_streaming_delay',
	'50ms');
$node_standby->restart();
reconnect_and_clear();


# Check that expected number of conflicts show in pg_stat_database. Needs to
# be tested before database is dropped, for obvious reasons.
is( $node_standby->safe_psql(
		$test_db,
		qq[SELECT conflicts FROM pg_stat_database WHERE datname='$test_db';]),
	$expected_conflicts,
	qq[$expected_conflicts recovery conflicts shown in pg_stat_database]);


## RECOVERY CONFLICT 6: Database conflict
$sect = "database conflict";

$node_primary->safe_psql('postgres', qq[DROP DATABASE $test_db;]);

$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

check_conflict_log("User was connected to a database that must be dropped");


# explicitly shut down psql instances gracefully - to avoid hangs or worse on
# windows
$psql_standby{stdin} .= "\\q\n";
$psql_standby{run}->finish;

$node_standby->stop();
$node_primary->stop();


done_testing();


sub pump_until_standby
{
	my $match = shift;

	return pump_until($psql_standby{run}, $psql_timeout,
		\$psql_standby{stdout}, $match);
}

sub reconnect_and_clear
{
	# If psql isn't dead already, tell it to quit as \q, when already dead,
	# causes IPC::Run to unhelpfully error out with "ack Broken pipe:".
	$psql_standby{run}->pump_nb();
	if ($psql_standby{run}->pumpable())
	{
		$psql_standby{stdin} .= "\\q\n";
	}
	$psql_standby{run}->finish;

	# restart
	$psql_standby{run}->run();
	$psql_standby{stdin}  = '';
	$psql_standby{stdout} = '';

	# Run query to ensure connection has finished re-establishing
	$psql_standby{stdin} .= qq[SELECT 1;\n];
	die unless pump_until_standby(qr/^1$/m);
	$psql_standby{stdout} = '';
}

sub check_conflict_log
{
	my $message          = shift;
	my $old_log_location = $log_location;

	$log_location = $node_standby->wait_for_log(qr/$message/, $log_location);

	cmp_ok($log_location, '>', $old_log_location,
		"$sect: logfile contains terminated connection due to recovery conflict"
	);
}

sub check_conflict_stat
{
	my $conflict_type = shift;
	my $count         = $node_standby->safe_psql($test_db,
		qq[SELECT confl_$conflict_type FROM pg_stat_database_conflicts WHERE datname='$test_db';]
	);

	is($count, 1, "$sect: stats show conflict on standby");
}
