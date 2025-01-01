# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Tests that standbys:
# - drop stats for objects when the those records are replayed
# - persist stats across graceful restarts
# - discard stats after immediate / crash restarts

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf('postgresql.conf', "track_functions = 'all'");
$node_primary->start;

my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;


## Test that stats are cleaned up on standby after dropping table or function

my $sect = 'initial';

my ($dboid, $tableoid, $funcoid) =
  populate_standby_stats('postgres', 'public');
test_standby_func_tab_stats_status('postgres',
	$dboid, $tableoid, $funcoid, 't');

drop_table_by_oid('postgres', $tableoid);
drop_function_by_oid('postgres', $funcoid);

$sect = 'post drop';
$node_primary->wait_for_replay_catchup($node_standby);
test_standby_func_tab_stats_status('postgres',
	$dboid, $tableoid, $funcoid, 'f');


## Test that stats are cleaned up on standby after dropping indirectly

$sect = "schema creation";

$node_primary->safe_psql('postgres', "CREATE SCHEMA drop_schema_test1");
$node_primary->wait_for_replay_catchup($node_standby);

($dboid, $tableoid, $funcoid) =
  populate_standby_stats('postgres', 'drop_schema_test1');

test_standby_func_tab_stats_status('postgres',
	$dboid, $tableoid, $funcoid, 't');
$node_primary->safe_psql('postgres', "DROP SCHEMA drop_schema_test1 CASCADE");

$sect = "post schema drop";

$node_primary->wait_for_replay_catchup($node_standby);

# verify table and function stats removed from standby
test_standby_func_tab_stats_status('postgres',
	$dboid, $tableoid, $funcoid, 'f');


## Test that stats are cleaned up on standby after dropping database

$sect = "createdb";

$node_primary->safe_psql('postgres', "CREATE DATABASE test");
$node_primary->wait_for_replay_catchup($node_standby);

($dboid, $tableoid, $funcoid) = populate_standby_stats('test', 'public');

# verify stats are present
test_standby_func_tab_stats_status('test', $dboid, $tableoid, $funcoid, 't');
test_standby_db_stats_status('test', $dboid, 't');

$node_primary->safe_psql('postgres', "DROP DATABASE test");
$sect = "post dropdb";
$node_primary->wait_for_replay_catchup($node_standby);

# Test that the stats were cleaned up on standby
# Note that this connects to 'postgres' but provides the dboid of dropped db
# 'test' which we acquired previously
test_standby_func_tab_stats_status('postgres',
	$dboid, $tableoid, $funcoid, 'f');

test_standby_db_stats_status('postgres', $dboid, 'f');


## verify that stats persist across graceful restarts on a replica

# NB: Can't test database stats, they're immediately repopulated when
# reconnecting...
$sect = "pre restart";
($dboid, $tableoid, $funcoid) = populate_standby_stats('postgres', 'public');
test_standby_func_tab_stats_status('postgres',
	$dboid, $tableoid, $funcoid, 't');

$node_standby->restart();

$sect = "post non-immediate";

test_standby_func_tab_stats_status('postgres',
	$dboid, $tableoid, $funcoid, 't');

# but gone after an immediate restart
$node_standby->stop('immediate');
$node_standby->start();

$sect = "post immediate restart";

test_standby_func_tab_stats_status('postgres',
	$dboid, $tableoid, $funcoid, 'f');


done_testing();


sub populate_standby_stats
{
	my ($connect_db, $schema) = @_;

	# create objects on primary
	$node_primary->safe_psql($connect_db,
		"CREATE TABLE $schema.drop_tab_test1 AS SELECT generate_series(1,100) AS a"
	);
	$node_primary->safe_psql($connect_db,
		"CREATE FUNCTION $schema.drop_func_test1() RETURNS VOID AS 'select 2;' LANGUAGE SQL IMMUTABLE"
	);
	$node_primary->wait_for_replay_catchup($node_standby);

	# collect object oids
	my $dboid = $node_standby->safe_psql($connect_db,
		"SELECT oid FROM pg_database WHERE datname = '$connect_db'");
	my $tableoid = $node_standby->safe_psql($connect_db,
		"SELECT '$schema.drop_tab_test1'::regclass::oid");
	my $funcoid = $node_standby->safe_psql($connect_db,
		"SELECT '$schema.drop_func_test1()'::regprocedure::oid");

	# generate stats on standby
	$node_standby->safe_psql($connect_db,
		"SELECT * FROM $schema.drop_tab_test1");
	$node_standby->safe_psql($connect_db, "SELECT $schema.drop_func_test1()");

	return ($dboid, $tableoid, $funcoid);
}

sub drop_function_by_oid
{
	my ($connect_db, $funcoid) = @_;

	# Get function name from returned oid
	my $func_name = $node_primary->safe_psql($connect_db,
		"SELECT '$funcoid'::regprocedure");
	$node_primary->safe_psql($connect_db, "DROP FUNCTION $func_name");
}

sub drop_table_by_oid
{
	my ($connect_db, $tableoid) = @_;

	# Get table name from returned oid
	my $table_name =
	  $node_primary->safe_psql($connect_db, "SELECT '$tableoid'::regclass");
	$node_primary->safe_psql($connect_db, "DROP TABLE $table_name");
}

sub test_standby_func_tab_stats_status
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($connect_db, $dboid, $tableoid, $funcoid, $present) = @_;

	my %expected = (rel => $present, func => $present);
	my %stats;

	$stats{rel} = $node_standby->safe_psql($connect_db,
		"SELECT pg_stat_have_stats('relation', $dboid, $tableoid)");
	$stats{func} = $node_standby->safe_psql($connect_db,
		"SELECT pg_stat_have_stats('function', $dboid, $funcoid)");

	is_deeply(\%stats, \%expected, "$sect: standby stats as expected");

	return;
}

sub test_standby_db_stats_status
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($connect_db, $dboid, $present) = @_;

	is( $node_standby->safe_psql(
			$connect_db, "SELECT pg_stat_have_stats('database', $dboid, 0)"),
		$present,
		"$sect: standby db stats as expected");
}
