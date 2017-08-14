# Minimal test testing streaming replication
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 28;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->start;
my $backup_name = 'my_backup';

# Take backup
$node_master->backup($backup_name);

# Create streaming standby linking to master
my $node_standby_1 = get_new_node('standby_1');
$node_standby_1->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby_1->start;

# Take backup of standby 1 (not mandatory, but useful to check if
# pg_basebackup works on a standby).
$node_standby_1->backup($backup_name);

# Take a second backup of the standby while the master is offline.
$node_master->stop;
$node_standby_1->backup('my_backup_2');
$node_master->start;

# Create second standby node linking to standby 1
my $node_standby_2 = get_new_node('standby_2');
$node_standby_2->init_from_backup($node_standby_1, $backup_name,
	has_streaming => 1);
$node_standby_2->start;

# Create some content on master and check its presence in standby 1
$node_master->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1002) AS a");

# Wait for standbys to catch up
$node_master->wait_for_catchup($node_standby_1, 'replay',
	$node_master->lsn('insert'));
$node_standby_1->wait_for_catchup($node_standby_2, 'replay',
	$node_standby_1->lsn('replay'));

my $result =
  $node_standby_1->safe_psql('postgres', "SELECT count(*) FROM tab_int");
print "standby 1: $result\n";
is($result, qq(1002), 'check streamed content on standby 1');

$result =
  $node_standby_2->safe_psql('postgres', "SELECT count(*) FROM tab_int");
print "standby 2: $result\n";
is($result, qq(1002), 'check streamed content on standby 2');

# Check that only READ-only queries can run on standbys
is($node_standby_1->psql('postgres', 'INSERT INTO tab_int VALUES (1)'),
	3, 'read-only queries on standby 1');
is($node_standby_2->psql('postgres', 'INSERT INTO tab_int VALUES (1)'),
	3, 'read-only queries on standby 2');

# Tests for connection parameter target_session_attrs
note "testing connection parameter \"target_session_attrs\"";

# Routine designed to run tests on the connection parameter
# target_session_attrs with multiple nodes.
sub test_target_session_attrs
{
	my $node1       = shift;
	my $node2       = shift;
	my $target_node = shift;
	my $mode        = shift;
	my $status      = shift;

	my $node1_host = $node1->host;
	my $node1_port = $node1->port;
	my $node1_name = $node1->name;
	my $node2_host = $node2->host;
	my $node2_port = $node2->port;
	my $node2_name = $node2->name;

	my $target_name = $target_node->name;

	# Build connection string for connection attempt.
	my $connstr = "host=$node1_host,$node2_host ";
	$connstr .= "port=$node1_port,$node2_port ";
	$connstr .= "target_session_attrs=$mode";

	# The client used for the connection does not matter, only the backend
	# point does.
	my ($ret, $stdout, $stderr) =
	  $node1->psql('postgres', 'SHOW port;',
		extra_params => [ '-d', $connstr ]);
	is( $status == $ret && $stdout eq $target_node->port,
		1,
"connect to node $target_name if mode \"$mode\" and $node1_name,$node2_name listed"
	);
}

# Connect to master in "read-write" mode with master,standby1 list.
test_target_session_attrs($node_master, $node_standby_1, $node_master,
	"read-write", 0);

# Connect to master in "read-write" mode with standby1,master list.
test_target_session_attrs($node_standby_1, $node_master, $node_master,
	"read-write", 0);

# Connect to master in "any" mode with master,standby1 list.
test_target_session_attrs($node_master, $node_standby_1, $node_master, "any",
	0);

# Connect to standby1 in "any" mode with standby1,master list.
test_target_session_attrs($node_standby_1, $node_master, $node_standby_1,
	"any", 0);

note "switching to physical replication slot";

# Switch to using a physical replication slot. We can do this without a new
# backup since physical slots can go backwards if needed. Do so on both
# standbys. Since we're going to be testing things that affect the slot state,
# also increase the standby feedback interval to ensure timely updates.
my ($slotname_1, $slotname_2) = ('standby_1', 'standby_2');
$node_master->append_conf('postgresql.conf', "max_replication_slots = 4");
$node_master->restart;
is( $node_master->psql(
		'postgres',
		qq[SELECT pg_create_physical_replication_slot('$slotname_1');]),
	0,
	'physical slot created on master');
$node_standby_1->append_conf('recovery.conf',
	"primary_slot_name = $slotname_1");
$node_standby_1->append_conf('postgresql.conf',
	"wal_receiver_status_interval = 1");
$node_standby_1->append_conf('postgresql.conf', "max_replication_slots = 4");
$node_standby_1->restart;
is( $node_standby_1->psql(
		'postgres',
		qq[SELECT pg_create_physical_replication_slot('$slotname_2');]),
	0,
	'physical slot created on intermediate replica');
$node_standby_2->append_conf('recovery.conf',
	"primary_slot_name = $slotname_2");
$node_standby_2->append_conf('postgresql.conf',
	"wal_receiver_status_interval = 1");
$node_standby_2->restart;

# Fetch xmin columns from slot's pg_replication_slots row, after waiting for
# given boolean condition to be true to ensure we've reached a quiescent state
sub get_slot_xmins
{
	my ($node, $slotname, $check_expr) = @_;

	$node->poll_query_until(
		'postgres', qq[
		SELECT $check_expr
		FROM pg_catalog.pg_replication_slots
		WHERE slot_name = '$slotname';
	]) or die "Timed out waiting for slot xmins to advance";

	my $slotinfo = $node->slot($slotname);
	return ($slotinfo->{'xmin'}, $slotinfo->{'catalog_xmin'});
}

# There's no hot standby feedback and there are no logical slots on either peer
# so xmin and catalog_xmin should be null on both slots.
my ($xmin, $catalog_xmin) = get_slot_xmins($node_master, $slotname_1,
	"xmin IS NULL AND catalog_xmin IS NULL");
is($xmin, '', 'xmin of non-cascaded slot null with no hs_feedback');
is($catalog_xmin, '',
	'catalog xmin of non-cascaded slot null with no hs_feedback');

($xmin, $catalog_xmin) = get_slot_xmins($node_standby_1, $slotname_2,
	"xmin IS NULL AND catalog_xmin IS NULL");
is($xmin, '', 'xmin of cascaded slot null with no hs_feedback');
is($catalog_xmin, '',
	'catalog xmin of cascaded slot null with no hs_feedback');

# Replication still works?
$node_master->safe_psql('postgres', 'CREATE TABLE replayed(val integer);');

sub replay_check
{
	my $newval = $node_master->safe_psql('postgres',
'INSERT INTO replayed(val) SELECT coalesce(max(val),0) + 1 AS newval FROM replayed RETURNING val'
	);
	$node_master->wait_for_catchup($node_standby_1, 'replay',
		$node_master->lsn('insert'));
	$node_standby_1->wait_for_catchup($node_standby_2, 'replay',
		$node_standby_1->lsn('replay'));
	$node_standby_1->safe_psql('postgres',
		qq[SELECT 1 FROM replayed WHERE val = $newval])
	  or die "standby_1 didn't replay master value $newval";
	$node_standby_2->safe_psql('postgres',
		qq[SELECT 1 FROM replayed WHERE val = $newval])
	  or die "standby_2 didn't replay standby_1 value $newval";
}

replay_check();

note "enabling hot_standby_feedback";

# Enable hs_feedback. The slot should gain an xmin. We set the status interval
# so we'll see the results promptly.
$node_standby_1->safe_psql('postgres',
	'ALTER SYSTEM SET hot_standby_feedback = on;');
$node_standby_1->reload;
$node_standby_2->safe_psql('postgres',
	'ALTER SYSTEM SET hot_standby_feedback = on;');
$node_standby_2->reload;
replay_check();

($xmin, $catalog_xmin) = get_slot_xmins($node_master, $slotname_1,
	"xmin IS NOT NULL AND catalog_xmin IS NULL");
isnt($xmin, '', 'xmin of non-cascaded slot non-null with hs feedback');
is($catalog_xmin, '',
	'catalog xmin of non-cascaded slot still null with hs_feedback');

my ($xmin1, $catalog_xmin1) = get_slot_xmins($node_standby_1, $slotname_2,
	"xmin IS NOT NULL AND catalog_xmin IS NULL");
isnt($xmin1, '', 'xmin of cascaded slot non-null with hs feedback');
is($catalog_xmin1, '',
	'catalog xmin of cascaded slot still null with hs_feedback');

note "doing some work to advance xmin";
$node_master->safe_psql(
	'postgres', q{
do $$
begin
  for i in 10000..11000 loop
    -- use an exception block so that each iteration eats an XID
    begin
      insert into tab_int values (i);
    exception
      when division_by_zero then null;
    end;
  end loop;
end$$;
});

$node_master->safe_psql('postgres', 'VACUUM;');
$node_master->safe_psql('postgres', 'CHECKPOINT;');

my ($xmin2, $catalog_xmin2) =
  get_slot_xmins($node_master, $slotname_1, "xmin <> '$xmin'");
note "master slot's new xmin $xmin2, old xmin $xmin";
isnt($xmin2, $xmin, 'xmin of non-cascaded slot with hs feedback has changed');
is($catalog_xmin2, '',
	'catalog xmin of non-cascaded slot still null with hs_feedback unchanged'
);

($xmin2, $catalog_xmin2) =
  get_slot_xmins($node_standby_1, $slotname_2, "xmin <> '$xmin1'");
note "standby_1 slot's new xmin $xmin2, old xmin $xmin1";
isnt($xmin2, $xmin1, 'xmin of cascaded slot with hs feedback has changed');
is($catalog_xmin2, '',
	'catalog xmin of cascaded slot still null with hs_feedback unchanged');

note "disabling hot_standby_feedback";

# Disable hs_feedback. Xmin should be cleared.
$node_standby_1->safe_psql('postgres',
	'ALTER SYSTEM SET hot_standby_feedback = off;');
$node_standby_1->reload;
$node_standby_2->safe_psql('postgres',
	'ALTER SYSTEM SET hot_standby_feedback = off;');
$node_standby_2->reload;
replay_check();

($xmin, $catalog_xmin) = get_slot_xmins($node_master, $slotname_1,
	"xmin IS NULL AND catalog_xmin IS NULL");
is($xmin, '', 'xmin of non-cascaded slot null with hs feedback reset');
is($catalog_xmin, '',
	'catalog xmin of non-cascaded slot still null with hs_feedback reset');

($xmin, $catalog_xmin) = get_slot_xmins($node_standby_1, $slotname_2,
	"xmin IS NULL AND catalog_xmin IS NULL");
is($xmin, '', 'xmin of cascaded slot null with hs feedback reset');
is($catalog_xmin, '',
	'catalog xmin of cascaded slot still null with hs_feedback reset');

note "re-enabling hot_standby_feedback and disabling while stopped";
$node_standby_2->safe_psql('postgres',
	'ALTER SYSTEM SET hot_standby_feedback = on;');
$node_standby_2->reload;

$node_master->safe_psql('postgres', qq[INSERT INTO tab_int VALUES (11000);]);
replay_check();

$node_standby_2->safe_psql('postgres',
	'ALTER SYSTEM SET hot_standby_feedback = off;');
$node_standby_2->stop;

($xmin, $catalog_xmin) =
  get_slot_xmins($node_standby_1, $slotname_2, "xmin IS NOT NULL");
isnt($xmin, '', 'xmin of cascaded slot non-null with postgres shut down');

# Xmin from a previous run should be cleared on startup.
$node_standby_2->start;

($xmin, $catalog_xmin) =
  get_slot_xmins($node_standby_1, $slotname_2, "xmin IS NULL");
is($xmin, '',
	'xmin of cascaded slot reset after startup with hs feedback reset');
