
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test for recovery targets: name, timestamp, XID
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Create and test a standby from given backup, with a certain recovery target.
# Choose $until_lsn later than the transaction commit that causes the row
# count to reach $num_rows, yet not later than the recovery target.
sub test_recovery_standby
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $test_name = shift;
	my $node_name = shift;
	my $node_primary = shift;
	my $recovery_params = shift;
	my $num_rows = shift;
	my $until_lsn = shift;

	my $node_standby = PostgreSQL::Test::Cluster->new($node_name);
	$node_standby->init_from_backup($node_primary, 'my_backup',
		has_restoring => 1);

	foreach my $param_item (@$recovery_params)
	{
		$node_standby->append_conf('postgresql.conf', qq($param_item));
	}

	$node_standby->start;

	# Wait until standby has replayed enough data
	my $caughtup_query =
	  "SELECT '$until_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
	$node_standby->poll_query_until('postgres', $caughtup_query)
	  or die "Timed out while waiting for standby to catch up";

	# Create some content on primary and check its presence in standby
	my $result =
	  $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int");
	is($result, qq($num_rows), "check standby content for $test_name");

	# Stop standby node
	$node_standby->teardown_node;

	return;
}

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(has_archiving => 1, allows_streaming => 1);

# Bump the transaction ID epoch.  This is useful to stress the portability
# of recovery_target_xid parsing.
system_or_bail('pg_resetwal', '--epoch', '1', $node_primary->data_dir);

# Start it
$node_primary->start;

# Create data before taking the backup, aimed at testing
# recovery_target = 'immediate'
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1000) AS a");
my $lsn1 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

# Take backup from which all operations will be run
$node_primary->backup('my_backup');

# Insert some data with used as a replay reference, with a recovery
# target TXID.
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(1001,2000))");
my $ret = $node_primary->safe_psql('postgres',
	"SELECT pg_current_wal_lsn(), pg_current_xact_id();");
my ($lsn2, $recovery_txid) = split /\|/, $ret;

# More data, with recovery target timestamp
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(2001,3000))");
my $lsn3 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
my $recovery_time = $node_primary->safe_psql('postgres', "SELECT now()");

# Even more data, this time with a recovery target name
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(3001,4000))");
my $recovery_name = "my_target";
my $lsn4 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
$node_primary->safe_psql('postgres',
	"SELECT pg_create_restore_point('$recovery_name');");

# And now for a recovery target LSN
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(4001,5000))");
my $lsn5 = my $recovery_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");

$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(5001,6000))");

# Force archiving of WAL file
$node_primary->safe_psql('postgres', "SELECT pg_switch_wal()");

# Test recovery targets
my @recovery_params = ("recovery_target = 'immediate'");
test_recovery_standby('immediate target',
	'standby_1', $node_primary, \@recovery_params, "1000", $lsn1);
@recovery_params = ("recovery_target_xid = '$recovery_txid'");
test_recovery_standby('XID', 'standby_2', $node_primary, \@recovery_params,
	"2000", $lsn2);
@recovery_params = ("recovery_target_time = '$recovery_time'");
test_recovery_standby('time', 'standby_3', $node_primary, \@recovery_params,
	"3000", $lsn3);
@recovery_params = ("recovery_target_name = '$recovery_name'");
test_recovery_standby('name', 'standby_4', $node_primary, \@recovery_params,
	"4000", $lsn4);
@recovery_params = ("recovery_target_lsn = '$recovery_lsn'");
test_recovery_standby('LSN', 'standby_5', $node_primary, \@recovery_params,
	"5000", $lsn5);

# Multiple targets
#
# Multiple conflicting settings are not allowed, but setting the same
# parameter multiple times or unsetting a parameter and setting a
# different one is allowed.

@recovery_params = (
	"recovery_target_name = '$recovery_name'",
	"recovery_target_name = ''",
	"recovery_target_time = '$recovery_time'");
test_recovery_standby('multiple overriding settings',
	'standby_6', $node_primary, \@recovery_params, "3000", $lsn3);

my $node_standby = PostgreSQL::Test::Cluster->new('standby_7');
$node_standby->init_from_backup($node_primary, 'my_backup',
	has_restoring => 1);
$node_standby->append_conf(
	'postgresql.conf', "recovery_target_name = '$recovery_name'
recovery_target_time = '$recovery_time'");

my $res = run_log(
	[
		'pg_ctl', '-D', $node_standby->data_dir, '-l',
		$node_standby->logfile, 'start'
	]);
ok(!$res, 'invalid recovery startup fails');

my $logfile = slurp_file($node_standby->logfile());
ok($logfile =~ qr/multiple recovery targets specified/,
	'multiple conflicting settings');

# Check behavior when recovery ends before target is reached

$node_standby = PostgreSQL::Test::Cluster->new('standby_8');
$node_standby->init_from_backup(
	$node_primary, 'my_backup',
	has_restoring => 1,
	standby => 0);
$node_standby->append_conf('postgresql.conf',
	"recovery_target_name = 'does_not_exist'");

run_log(
	[
		'pg_ctl', '-D', $node_standby->data_dir, '-l',
		$node_standby->logfile, 'start'
	]);

# wait for postgres to terminate
foreach my $i (0 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	last if !-f $node_standby->data_dir . '/postmaster.pid';
	usleep(100_000);
}
$logfile = slurp_file($node_standby->logfile());
ok( $logfile =~
	  qr/FATAL: .* recovery ended before configured recovery target was reached/,
	'recovery end before target reached is a fatal error');

done_testing();
