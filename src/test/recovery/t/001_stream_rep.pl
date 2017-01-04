# Minimal test testing streaming replication
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 22;

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
$node_master->wait_for_catchup($node_standby_1, 'replay', $node_master->lsn('insert'));
$node_standby_1->wait_for_catchup($node_standby_2, 'replay', $node_standby_1->lsn('replay'));

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

diag "switching to physical replication slot";
# Switch to using a physical replication slot. We can do this without a new
# backup since physical slots can go backwards if needed. Do so on both
# standbys. Since we're going to be testing things that affect the slot state,
# also increase the standby feedback interval to ensure timely updates.
my ($slotname_1, $slotname_2) = ('standby_1', 'standby_2');
$node_master->append_conf('postgresql.conf', "max_replication_slots = 4\n");
$node_master->restart;
is($node_master->psql('postgres', qq[SELECT pg_create_physical_replication_slot('$slotname_1');]), 0, 'physical slot created on master');
$node_standby_1->append_conf('recovery.conf', "primary_slot_name = $slotname_1\n");
$node_standby_1->append_conf('postgresql.conf', "wal_receiver_status_interval = 1\n");
$node_standby_1->append_conf('postgresql.conf', "max_replication_slots = 4\n");
$node_standby_1->restart;
is($node_standby_1->psql('postgres', qq[SELECT pg_create_physical_replication_slot('$slotname_2');]), 0, 'physical slot created on intermediate replica');
$node_standby_2->append_conf('recovery.conf', "primary_slot_name = $slotname_2\n");
$node_standby_2->append_conf('postgresql.conf', "wal_receiver_status_interval = 1\n");
$node_standby_2->restart;

sub get_slot_xmins
{
	my ($node, $slotname) = @_;
	my $slotinfo = $node->slot($slotname);
	return ($slotinfo->{'xmin'}, $slotinfo->{'catalog_xmin'});
}

# There's no hot standby feedback and there are no logical slots on either peer
# so xmin and catalog_xmin should be null on both slots.
my ($xmin, $catalog_xmin) = get_slot_xmins($node_master, $slotname_1);
is($xmin, '', 'non-cascaded slot xmin null with no hs_feedback');
is($catalog_xmin, '', 'non-cascaded slot xmin null with no hs_feedback');

($xmin, $catalog_xmin) = get_slot_xmins($node_standby_1, $slotname_2);
is($xmin, '', 'cascaded slot xmin null with no hs_feedback');
is($catalog_xmin, '', 'cascaded slot xmin null with no hs_feedback');

# Replication still works?
$node_master->safe_psql('postgres', 'CREATE TABLE replayed(val integer);');

sub replay_check
{
	my $newval = $node_master->safe_psql('postgres', 'INSERT INTO replayed(val) SELECT coalesce(max(val),0) + 1 AS newval FROM replayed RETURNING val');
	$node_master->wait_for_catchup($node_standby_1, 'replay', $node_master->lsn('insert'));
	$node_standby_1->wait_for_catchup($node_standby_2, 'replay', $node_standby_1->lsn('replay'));
	$node_standby_1->safe_psql('postgres', qq[SELECT 1 FROM replayed WHERE val = $newval])
		or die "standby_1 didn't replay master value $newval";
	$node_standby_2->safe_psql('postgres', qq[SELECT 1 FROM replayed WHERE val = $newval])
		or die "standby_2 didn't replay standby_1 value $newval";
}

replay_check();

diag "enabling hot_standby_feedback";
# Enable hs_feedback. The slot should gain an xmin. We set the status interval
# so we'll see the results promptly.
$node_standby_1->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = on;');
$node_standby_1->reload;
$node_standby_2->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = on;');
$node_standby_2->reload;
replay_check();
sleep(2);

($xmin, $catalog_xmin) = get_slot_xmins($node_master, $slotname_1);
isnt($xmin, '', 'non-cascaded slot xmin non-null with hs feedback');
is($catalog_xmin, '', 'non-cascaded slot xmin still null with hs_feedback');

($xmin, $catalog_xmin) = get_slot_xmins($node_standby_1, $slotname_2);
isnt($xmin, '', 'cascaded slot xmin non-null with hs feedback');
is($catalog_xmin, '', 'cascaded slot xmin still null with hs_feedback');

diag "doing some work to advance xmin";
for my $i (10000..11000) {
	$node_master->safe_psql('postgres', qq[INSERT INTO tab_int VALUES ($i);]);
}
$node_master->safe_psql('postgres', 'VACUUM;');
$node_master->safe_psql('postgres', 'CHECKPOINT;');

my ($xmin2, $catalog_xmin2) = get_slot_xmins($node_master, $slotname_1);
diag "new xmin $xmin2, old xmin $xmin";
isnt($xmin2, $xmin, 'non-cascaded slot xmin with hs feedback has changed');
is($catalog_xmin2, '', 'non-cascaded slot xmin still null with hs_feedback unchanged');

($xmin2, $catalog_xmin2) = get_slot_xmins($node_standby_1, $slotname_2);
diag "new xmin $xmin2, old xmin $xmin";
isnt($xmin2, $xmin, 'cascaded slot xmin with hs feedback has changed');
is($catalog_xmin2, '', 'cascaded slot xmin still null with hs_feedback unchanged');

diag "disabling hot_standby_feedback";
# Disable hs_feedback. Xmin should be cleared.
$node_standby_1->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = off;');
$node_standby_1->reload;
$node_standby_2->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = off;');
$node_standby_2->reload;
replay_check();
sleep(2);

($xmin, $catalog_xmin) = get_slot_xmins($node_master, $slotname_1);
is($xmin, '', 'non-cascaded slot xmin null with hs feedback reset');
is($catalog_xmin, '', 'non-cascaded slot xmin still null with hs_feedback reset');

($xmin, $catalog_xmin) = get_slot_xmins($node_standby_1, $slotname_2);
is($xmin, '', 'cascaded slot xmin null with hs feedback reset');
is($catalog_xmin, '', 'cascaded slot xmin still null with hs_feedback reset');
