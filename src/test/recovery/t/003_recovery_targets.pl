# Test for recovery targets: name, timestamp, XID
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 7;

# Create and test a standby from given backup, with a certain
# recovery target.
sub test_recovery_standby
{
	my $test_name       = shift;
	my $node_name       = shift;
	my $node_master     = shift;
	my $recovery_params = shift;
	my $num_rows        = shift;
	my $until_lsn       = shift;

	my $node_standby = get_new_node($node_name);
	$node_standby->init_from_backup($node_master, 'my_backup',
		has_restoring => 1);

	foreach my $param_item (@$recovery_params)
	{
		$node_standby->append_conf(
			'recovery.conf',
			qq($param_item
));
	}

	$node_standby->start;

	# Wait until standby has replayed enough data
	my $caughtup_query =
	  "SELECT '$until_lsn'::pg_lsn <= pg_last_xlog_replay_location()";
	$node_standby->poll_query_until('postgres', $caughtup_query)
	  or die "Timed out while waiting for standby to catch up";

	# Create some content on master and check its presence in standby
	my $result =
	  $node_standby->psql('postgres', "SELECT count(*) FROM tab_int");
	is($result, qq($num_rows), "check standby content for $test_name");

	# Stop standby node
	$node_standby->teardown_node;
}

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(has_archiving => 1, allows_streaming => 1);

# Start it
$node_master->start;

# Create data before taking the backup, aimed at testing
# recovery_target = 'immediate'
$node_master->psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1000) AS a");
my $lsn1 =
  $node_master->psql('postgres', "SELECT pg_current_xlog_location();");

# Take backup from which all operations will be run
$node_master->backup('my_backup');

# Insert some data with used as a replay reference, with a recovery
# target TXID.
$node_master->psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(1001,2000))");
my $recovery_txid = $node_master->psql('postgres', "SELECT txid_current()");
my $lsn2 =
  $node_master->psql('postgres', "SELECT pg_current_xlog_location();");

# More data, with recovery target timestamp
$node_master->psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(2001,3000))");
my $recovery_time = $node_master->psql('postgres', "SELECT now()");
my $lsn3 =
  $node_master->psql('postgres', "SELECT pg_current_xlog_location();");

# Even more data, this time with a recovery target name
$node_master->psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(3001,4000))");
my $recovery_name = "my_target";
my $lsn4 =
  $node_master->psql('postgres', "SELECT pg_current_xlog_location();");
$node_master->psql('postgres',
	"SELECT pg_create_restore_point('$recovery_name'");

# Force archiving of WAL file
$node_master->psql('postgres', "SELECT pg_switch_xlog()");

# Test recovery targets
my @recovery_params = ("recovery_target = 'immediate'");
test_recovery_standby('immediate target',
	'standby_1', $node_master, \@recovery_params, "1000", $lsn1);
@recovery_params = ("recovery_target_xid = '$recovery_txid'");
test_recovery_standby('XID', 'standby_2', $node_master, \@recovery_params,
	"2000", $lsn2);
@recovery_params = ("recovery_target_time = '$recovery_time'");
test_recovery_standby('Time', 'standby_3', $node_master, \@recovery_params,
	"3000", $lsn3);
@recovery_params = ("recovery_target_name = '$recovery_name'");
test_recovery_standby('Name', 'standby_4', $node_master, \@recovery_params,
	"4000", $lsn4);

# Multiple targets
# Last entry has priority (note that an array respects the order of items
# not hashes).
@recovery_params = (
	"recovery_target_name = '$recovery_name'",
	"recovery_target_xid  = '$recovery_txid'",
	"recovery_target_time = '$recovery_time'");
test_recovery_standby('Name + XID + Time',
	'standby_5', $node_master, \@recovery_params, "3000", $lsn3);
@recovery_params = (
	"recovery_target_time = '$recovery_time'",
	"recovery_target_name = '$recovery_name'",
	"recovery_target_xid  = '$recovery_txid'");
test_recovery_standby('Time + Name + XID',
	'standby_6', $node_master, \@recovery_params, "2000", $lsn2);
@recovery_params = (
	"recovery_target_xid  = '$recovery_txid'",
	"recovery_target_time = '$recovery_time'",
	"recovery_target_name = '$recovery_name'");
test_recovery_standby('XID + Time + Name',
	'standby_7', $node_master, \@recovery_params, "4000", $lsn4);
