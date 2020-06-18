# Minimal test testing streaming replication
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 36;

# Initialize master node
my $node_master = get_new_node('master');
# A specific role is created to perform some tests related to replication,
# and it needs proper authentication configuration.
$node_master->init(
	allows_streaming => 1,
	auth_extra       => [ '--create-role', 'repl_role' ]);
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

	return;
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

# Test for SHOW commands using a WAL sender connection with a replication
# role.
note "testing SHOW commands for replication connection";

$node_master->psql(
	'postgres', "
CREATE ROLE repl_role REPLICATION LOGIN;
GRANT pg_read_all_settings TO repl_role;");
my $master_host    = $node_master->host;
my $master_port    = $node_master->port;
my $connstr_common = "host=$master_host port=$master_port user=repl_role";
my $connstr_rep    = "$connstr_common replication=1";
my $connstr_db     = "$connstr_common replication=database dbname=postgres";

# Test SHOW ALL
my ($ret, $stdout, $stderr) = $node_master->psql(
	'postgres', 'SHOW ALL;',
	on_error_die => 1,
	extra_params => [ '-d', $connstr_rep ]);
ok($ret == 0, "SHOW ALL with replication role and physical replication");
($ret, $stdout, $stderr) = $node_master->psql(
	'postgres', 'SHOW ALL;',
	on_error_die => 1,
	extra_params => [ '-d', $connstr_db ]);
ok($ret == 0, "SHOW ALL with replication role and logical replication");

# Test SHOW with a user-settable parameter
($ret, $stdout, $stderr) = $node_master->psql(
	'postgres', 'SHOW work_mem;',
	on_error_die => 1,
	extra_params => [ '-d', $connstr_rep ]);
ok( $ret == 0,
	"SHOW with user-settable parameter, replication role and physical replication"
);
($ret, $stdout, $stderr) = $node_master->psql(
	'postgres', 'SHOW work_mem;',
	on_error_die => 1,
	extra_params => [ '-d', $connstr_db ]);
ok( $ret == 0,
	"SHOW with user-settable parameter, replication role and logical replication"
);

# Test SHOW with a superuser-settable parameter
($ret, $stdout, $stderr) = $node_master->psql(
	'postgres', 'SHOW primary_conninfo;',
	on_error_die => 1,
	extra_params => [ '-d', $connstr_rep ]);
ok( $ret == 0,
	"SHOW with superuser-settable parameter, replication role and physical replication"
);
($ret, $stdout, $stderr) = $node_master->psql(
	'postgres', 'SHOW primary_conninfo;',
	on_error_die => 1,
	extra_params => [ '-d', $connstr_db ]);
ok( $ret == 0,
	"SHOW with superuser-settable parameter, replication role and logical replication"
);

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
$node_standby_1->append_conf('postgresql.conf',
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
$node_standby_2->append_conf('postgresql.conf',
	"primary_slot_name = $slotname_2");
$node_standby_2->append_conf('postgresql.conf',
	"wal_receiver_status_interval = 1");
# should be able change primary_slot_name without restart
# will wait effect in get_slot_xmins above
$node_standby_2->reload;

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
	return;
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

note "check change primary_conninfo without restart";
$node_standby_2->append_conf('postgresql.conf', "primary_slot_name = ''");
$node_standby_2->enable_streaming($node_master);
$node_standby_2->reload;

# be sure do not streaming from cascade
$node_standby_1->stop;

my $newval = $node_master->safe_psql('postgres',
	'INSERT INTO replayed(val) SELECT coalesce(max(val),0) + 1 AS newval FROM replayed RETURNING val'
);
$node_master->wait_for_catchup($node_standby_2, 'replay',
	$node_master->lsn('insert'));
my $is_replayed = $node_standby_2->safe_psql('postgres',
	qq[SELECT 1 FROM replayed WHERE val = $newval]);
is($is_replayed, qq(1), "standby_2 didn't replay master value $newval");

# Drop any existing slots on the primary, for the follow-up tests.
$node_master->safe_psql('postgres',
	"SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots;");

# Test physical slot advancing and its durability.  Create a new slot on
# the primary, not used by any of the standbys. This reserves WAL at creation.
my $phys_slot = 'phys_slot';
$node_master->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('$phys_slot', true);");
# Generate some WAL, and switch to a new segment, used to check that
# the previous segment is correctly getting recycled as the slot advancing
# would recompute the minimum LSN calculated across all slots.
my $segment_removed = $node_master->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');
chomp($segment_removed);
$node_master->psql(
	'postgres', "
	CREATE TABLE tab_phys_slot (a int);
	INSERT INTO tab_phys_slot VALUES (generate_series(1,10));
	SELECT pg_switch_wal();");
my $current_lsn =
  $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
chomp($current_lsn);
my $psql_rc = $node_master->psql('postgres',
	"SELECT pg_replication_slot_advance('$phys_slot', '$current_lsn'::pg_lsn);"
);
is($psql_rc, '0', 'slot advancing with physical slot');
my $phys_restart_lsn_pre = $node_master->safe_psql('postgres',
	"SELECT restart_lsn from pg_replication_slots WHERE slot_name = '$phys_slot';"
);
chomp($phys_restart_lsn_pre);
# Slot advance should persist across clean restarts.
$node_master->restart;
my $phys_restart_lsn_post = $node_master->safe_psql('postgres',
	"SELECT restart_lsn from pg_replication_slots WHERE slot_name = '$phys_slot';"
);
chomp($phys_restart_lsn_post);
ok( ($phys_restart_lsn_pre cmp $phys_restart_lsn_post) == 0,
	"physical slot advance persists across restarts");

# Check if the previous segment gets correctly recycled after the
# server stopped cleanly, causing a shutdown checkpoint to be generated.
my $master_data = $node_master->data_dir;
ok(!-f "$master_data/pg_wal/$segment_removed",
	"WAL segment $segment_removed recycled after physical slot advancing");
