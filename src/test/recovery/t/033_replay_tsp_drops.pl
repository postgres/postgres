
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test replay of tablespace/database creation/drop

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

sub test_tablespace
{
	my ($strategy) = @_;

	my $node_primary = PostgreSQL::Test::Cluster->new("primary1_$strategy");
	$node_primary->init(allows_streaming => 1);
	$node_primary->start;
	$node_primary->psql(
		'postgres',
		qq[
			SET allow_in_place_tablespaces=on;
			CREATE TABLESPACE dropme_ts1 LOCATION '';
			CREATE TABLESPACE dropme_ts2 LOCATION '';
			CREATE TABLESPACE source_ts  LOCATION '';
			CREATE TABLESPACE target_ts  LOCATION '';
			CREATE DATABASE template_db IS_TEMPLATE = true;
			SELECT pg_create_physical_replication_slot('slot', true);
		]);
	my $backup_name = 'my_backup';
	$node_primary->backup($backup_name);

	my $node_standby = PostgreSQL::Test::Cluster->new("standby2_$strategy");
	$node_standby->init_from_backup($node_primary, $backup_name,
		has_streaming => 1);
	$node_standby->append_conf('postgresql.conf',
		"allow_in_place_tablespaces = on");
	$node_standby->append_conf('postgresql.conf', "primary_slot_name = slot");
	$node_standby->start;

	# Make sure the connection is made
	$node_primary->wait_for_catchup($node_standby, 'write');

	# Do immediate shutdown just after a sequence of CREATE DATABASE / DROP
	# DATABASE / DROP TABLESPACE. This causes CREATE DATABASE WAL records
	# to be applied to already-removed directories.
	my $query = q[
		CREATE DATABASE dropme_db1 WITH TABLESPACE dropme_ts1 STRATEGY=<STRATEGY>;
		CREATE TABLE t (a int) TABLESPACE dropme_ts2;
		CREATE DATABASE dropme_db2 WITH TABLESPACE dropme_ts2 STRATEGY=<STRATEGY>;
		CREATE DATABASE moveme_db TABLESPACE source_ts STRATEGY=<STRATEGY>;
		ALTER DATABASE moveme_db SET TABLESPACE target_ts;
		CREATE DATABASE newdb TEMPLATE template_db STRATEGY=<STRATEGY>;
		ALTER DATABASE template_db IS_TEMPLATE = false;
		DROP DATABASE dropme_db1;
		DROP TABLE t;
		DROP DATABASE dropme_db2; DROP TABLESPACE dropme_ts2;
		DROP TABLESPACE source_ts;
		DROP DATABASE template_db;
	];
	$query =~ s/<STRATEGY>/$strategy/g;

	$node_primary->safe_psql('postgres', $query);
	$node_primary->wait_for_catchup($node_standby, 'write');

	# show "create missing directory" log message
	$node_standby->safe_psql('postgres',
		"ALTER SYSTEM SET log_min_messages TO debug1;");
	$node_standby->stop('immediate');
	# Should restart ignoring directory creation error.
	is($node_standby->start(fail_ok => 1),
		1, "standby node started for $strategy");
	$node_standby->stop('immediate');
}

test_tablespace("FILE_COPY");
test_tablespace("WAL_LOG");

# Ensure that a missing tablespace directory during create database
# replay immediately causes panic if the standby has already reached
# consistent state (archive recovery is in progress).  This is
# effective only for CREATE DATABASE WITH STRATEGY=FILE_COPY.

my $node_primary = PostgreSQL::Test::Cluster->new('primary2');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

# Create tablespace
$node_primary->safe_psql(
	'postgres', q[
		SET allow_in_place_tablespaces=on;
		CREATE TABLESPACE ts1 LOCATION ''
			]);
$node_primary->safe_psql('postgres',
	"CREATE DATABASE db1 WITH TABLESPACE ts1 STRATEGY=FILE_COPY");

# Take backup
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);
my $node_standby = PostgreSQL::Test::Cluster->new('standby3');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->append_conf('postgresql.conf',
	"allow_in_place_tablespaces = on");
$node_standby->start;

# Make sure standby reached consistency and starts accepting connections
$node_standby->poll_query_until('postgres', 'SELECT 1', '1');

# Remove standby tablespace directory so it will be missing when
# replay resumes.
my $tspoid = $node_standby->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'ts1';");
my $tspdir = $node_standby->data_dir . "/pg_tblspc/$tspoid";
File::Path::rmtree($tspdir);

my $logstart = -s $node_standby->logfile;

# Create a database in the tablespace and a table in default tablespace
$node_primary->safe_psql(
	'postgres',
	q[
		CREATE TABLE should_not_replay_insertion(a int);
		CREATE DATABASE db2 WITH TABLESPACE ts1 STRATEGY=FILE_COPY;
		INSERT INTO should_not_replay_insertion VALUES (1);
	]);

# Standby should fail and should not silently skip replaying the wal
# In this test, PANIC turns into WARNING by allow_in_place_tablespaces.
# Check the log messages instead of confirming standby failure.
my $max_attempts = $PostgreSQL::Test::Utils::timeout_default * 10;
while ($max_attempts-- >= 0)
{
	last
	  if (
		$node_standby->log_contains(
			qr!WARNING: ( [A-Z0-9]+:)? creating missing directory: pg_tblspc/!,
			$logstart));
	usleep(100_000);
}
ok($max_attempts > 0, "invalid directory creation is detected");

done_testing();
