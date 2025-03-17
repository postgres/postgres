
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Minimal test testing streaming replication
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
# A specific role is created to perform some tests related to replication,
# and it needs proper authentication configuration.
$node_primary->init(
	allows_streaming => 1,
	auth_extra => [ '--create-role' => 'repl_role' ]);
$node_primary->start;
my $backup_name = 'my_backup';

# Take backup
$node_primary->backup($backup_name);

# Create streaming standby linking to primary
my $node_standby_1 = PostgreSQL::Test::Cluster->new('standby_1');
$node_standby_1->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby_1->start;

# Take backup of standby 1 (not mandatory, but useful to check if
# pg_basebackup works on a standby).
$node_standby_1->backup($backup_name);

# Take a second backup of the standby while the primary is offline.
$node_primary->stop;
$node_standby_1->backup('my_backup_2');
$node_primary->start;

# Create second standby node linking to standby 1
my $node_standby_2 = PostgreSQL::Test::Cluster->new('standby_2');
$node_standby_2->init_from_backup($node_standby_1, $backup_name,
	has_streaming => 1);
$node_standby_2->start;

# Create some content on primary and check its presence in standby nodes
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1002) AS a");

$node_primary->safe_psql(
	'postgres', q{
CREATE TABLE user_logins(id serial, who text);

CREATE FUNCTION on_login_proc() RETURNS EVENT_TRIGGER AS $$
BEGIN
  IF NOT pg_is_in_recovery() THEN
    INSERT INTO user_logins (who) VALUES (session_user);
  END IF;
  IF session_user = 'regress_hacker' THEN
    RAISE EXCEPTION 'You are not welcome!';
  END IF;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

CREATE EVENT TRIGGER on_login_trigger ON login EXECUTE FUNCTION on_login_proc();
ALTER EVENT TRIGGER on_login_trigger ENABLE ALWAYS;
});

# Wait for standbys to catch up
$node_primary->wait_for_replay_catchup($node_standby_1);
$node_standby_1->wait_for_replay_catchup($node_standby_2, $node_primary);

my $result =
  $node_standby_1->safe_psql('postgres', "SELECT count(*) FROM tab_int");
print "standby 1: $result\n";
is($result, qq(1002), 'check streamed content on standby 1');

$result =
  $node_standby_2->safe_psql('postgres', "SELECT count(*) FROM tab_int");
print "standby 2: $result\n";
is($result, qq(1002), 'check streamed content on standby 2');

# Likewise, but for a sequence
$node_primary->safe_psql('postgres',
	"CREATE SEQUENCE seq1; SELECT nextval('seq1')");

# Wait for standbys to catch up
$node_primary->wait_for_replay_catchup($node_standby_1);
$node_standby_1->wait_for_replay_catchup($node_standby_2, $node_primary);

$result = $node_standby_1->safe_psql('postgres', "SELECT * FROM seq1");
print "standby 1: $result\n";
is($result, qq(33|0|t), 'check streamed sequence content on standby 1');

$result = $node_standby_2->safe_psql('postgres', "SELECT * FROM seq1");
print "standby 2: $result\n";
is($result, qq(33|0|t), 'check streamed sequence content on standby 2');

# Check pg_sequence_last_value() returns NULL for unlogged sequence on standby
$node_primary->safe_psql('postgres',
	"CREATE UNLOGGED SEQUENCE ulseq; SELECT nextval('ulseq')");
$node_primary->wait_for_replay_catchup($node_standby_1);
is( $node_standby_1->safe_psql(
		'postgres',
		"SELECT pg_sequence_last_value('ulseq'::regclass) IS NULL"),
	't',
	'pg_sequence_last_value() on unlogged sequence on standby 1');

# Check that only READ-only queries can run on standbys
is($node_standby_1->psql('postgres', 'INSERT INTO tab_int VALUES (1)'),
	3, 'read-only queries on standby 1');
is($node_standby_2->psql('postgres', 'INSERT INTO tab_int VALUES (1)'),
	3, 'read-only queries on standby 2');

# Tests for connection parameter target_session_attrs
note "testing connection parameter \"target_session_attrs\"";

# Attempt to connect to $node1, then $node2, using target_session_attrs=$mode.
# Expect to connect to $target_node (undef for failure) with given $status.
sub test_target_session_attrs
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $node1 = shift;
	my $node2 = shift;
	my $target_node = shift;
	my $mode = shift;
	my $status = shift;

	my $node1_host = $node1->host;
	my $node1_port = $node1->port;
	my $node1_name = $node1->name;
	my $node2_host = $node2->host;
	my $node2_port = $node2->port;
	my $node2_name = $node2->name;
	my $target_port = undef;
	$target_port = $target_node->port if (defined $target_node);
	my $target_name = undef;
	$target_name = $target_node->name if (defined $target_node);

	# Build connection string for connection attempt.
	my $connstr = "host=$node1_host,$node2_host ";
	$connstr .= "port=$node1_port,$node2_port ";
	$connstr .= "target_session_attrs=$mode";

	# Attempt to connect, and if successful, get the server port number
	# we connected to.  Note we must pass the SQL command via the command
	# line not stdin, else Perl may spit up trying to write to stdin of
	# an already-failed psql process.
	my ($ret, $stdout, $stderr) = $node1->psql(
		'postgres',
		undef,
		extra_params => [
			'--dbname' => $connstr,
			'--command' => 'SHOW port;',
		]);
	if ($status == 0)
	{
		is( $status == $ret && $stdout eq $target_port,
			1,
			"connect to node $target_name with mode \"$mode\" and $node1_name,$node2_name listed"
		);
	}
	else
	{
		print "status = $status\n";
		print "ret = $ret\n";
		print "stdout = $stdout\n";
		print "stderr = $stderr\n";

		is( $status == $ret && !defined $target_node,
			1,
			"fail to connect with mode \"$mode\" and $node1_name,$node2_name listed"
		);
	}

	return;
}

# Connect to primary in "read-write" mode with primary,standby1 list.
test_target_session_attrs($node_primary, $node_standby_1, $node_primary,
	"read-write", 0);

# Connect to primary in "read-write" mode with standby1,primary list.
test_target_session_attrs($node_standby_1, $node_primary, $node_primary,
	"read-write", 0);

# Connect to primary in "any" mode with primary,standby1 list.
test_target_session_attrs($node_primary, $node_standby_1, $node_primary,
	"any", 0);

# Connect to standby1 in "any" mode with standby1,primary list.
test_target_session_attrs($node_standby_1, $node_primary, $node_standby_1,
	"any", 0);

# Connect to primary in "primary" mode with primary,standby1 list.
test_target_session_attrs($node_primary, $node_standby_1, $node_primary,
	"primary", 0);

# Connect to primary in "primary" mode with standby1,primary list.
test_target_session_attrs($node_standby_1, $node_primary, $node_primary,
	"primary", 0);

# Connect to standby1 in "read-only" mode with primary,standby1 list.
test_target_session_attrs($node_primary, $node_standby_1, $node_standby_1,
	"read-only", 0);

# Connect to standby1 in "read-only" mode with standby1,primary list.
test_target_session_attrs($node_standby_1, $node_primary, $node_standby_1,
	"read-only", 0);

# Connect to primary in "prefer-standby" mode with primary,primary list.
test_target_session_attrs($node_primary, $node_primary, $node_primary,
	"prefer-standby", 0);

# Connect to standby1 in "prefer-standby" mode with primary,standby1 list.
test_target_session_attrs($node_primary, $node_standby_1, $node_standby_1,
	"prefer-standby", 0);

# Connect to standby1 in "prefer-standby" mode with standby1,primary list.
test_target_session_attrs($node_standby_1, $node_primary, $node_standby_1,
	"prefer-standby", 0);

# Connect to standby1 in "standby" mode with primary,standby1 list.
test_target_session_attrs($node_primary, $node_standby_1, $node_standby_1,
	"standby", 0);

# Connect to standby1 in "standby" mode with standby1,primary list.
test_target_session_attrs($node_standby_1, $node_primary, $node_standby_1,
	"standby", 0);

# Fail to connect in "read-write" mode with standby1,standby2 list.
test_target_session_attrs($node_standby_1, $node_standby_2, undef,
	"read-write", 2);

# Fail to connect in "primary" mode with standby1,standby2 list.
test_target_session_attrs($node_standby_1, $node_standby_2, undef,
	"primary", 2);

# Fail to connect in "read-only" mode with primary,primary list.
test_target_session_attrs($node_primary, $node_primary, undef,
	"read-only", 2);

# Fail to connect in "standby" mode with primary,primary list.
test_target_session_attrs($node_primary, $node_primary, undef, "standby", 2);

# Test for SHOW commands using a WAL sender connection with a replication
# role.
note "testing SHOW commands for replication connection";

$node_primary->psql(
	'postgres', "
CREATE ROLE repl_role REPLICATION LOGIN;
GRANT pg_read_all_settings TO repl_role;");
my $primary_host = $node_primary->host;
my $primary_port = $node_primary->port;
my $connstr_common = "host=$primary_host port=$primary_port user=repl_role";
my $connstr_rep = "$connstr_common replication=1";
my $connstr_db = "$connstr_common replication=database dbname=postgres";

# Test SHOW ALL
my ($ret, $stdout, $stderr) = $node_primary->psql(
	'postgres', 'SHOW ALL;',
	on_error_die => 1,
	extra_params => [ '--dbname' => $connstr_rep ]);
ok($ret == 0, "SHOW ALL with replication role and physical replication");
($ret, $stdout, $stderr) = $node_primary->psql(
	'postgres', 'SHOW ALL;',
	on_error_die => 1,
	extra_params => [ '--dbname' => $connstr_db ]);
ok($ret == 0, "SHOW ALL with replication role and logical replication");

# Test SHOW with a user-settable parameter
($ret, $stdout, $stderr) = $node_primary->psql(
	'postgres', 'SHOW work_mem;',
	on_error_die => 1,
	extra_params => [ '--dbname' => $connstr_rep ]);
ok( $ret == 0,
	"SHOW with user-settable parameter, replication role and physical replication"
);
($ret, $stdout, $stderr) = $node_primary->psql(
	'postgres', 'SHOW work_mem;',
	on_error_die => 1,
	extra_params => [ '--dbname' => $connstr_db ]);
ok( $ret == 0,
	"SHOW with user-settable parameter, replication role and logical replication"
);

# Test SHOW with a superuser-settable parameter
($ret, $stdout, $stderr) = $node_primary->psql(
	'postgres', 'SHOW primary_conninfo;',
	on_error_die => 1,
	extra_params => [ '--dbname' => $connstr_rep ]);
ok( $ret == 0,
	"SHOW with superuser-settable parameter, replication role and physical replication"
);
($ret, $stdout, $stderr) = $node_primary->psql(
	'postgres', 'SHOW primary_conninfo;',
	on_error_die => 1,
	extra_params => [ '--dbname' => $connstr_db ]);
ok( $ret == 0,
	"SHOW with superuser-settable parameter, replication role and logical replication"
);

note "testing READ_REPLICATION_SLOT command for replication connection";

my $slotname = 'test_read_replication_slot_physical';

($ret, $stdout, $stderr) = $node_primary->psql(
	'postgres',
	'READ_REPLICATION_SLOT non_existent_slot;',
	extra_params => [ '--dbname' => $connstr_rep ]);
ok($ret == 0, "READ_REPLICATION_SLOT exit code 0 on success");
like($stdout, qr/^\|\|$/,
	"READ_REPLICATION_SLOT returns NULL values if slot does not exist");

$node_primary->psql(
	'postgres',
	"CREATE_REPLICATION_SLOT $slotname PHYSICAL RESERVE_WAL;",
	extra_params => [ '--dbname' => $connstr_rep ]);

($ret, $stdout, $stderr) = $node_primary->psql(
	'postgres',
	"READ_REPLICATION_SLOT $slotname;",
	extra_params => [ '--dbname' => $connstr_rep ]);
ok($ret == 0, "READ_REPLICATION_SLOT success with existing slot");
like($stdout, qr/^physical\|[^|]*\|1$/,
	"READ_REPLICATION_SLOT returns tuple with slot information");

$node_primary->psql(
	'postgres',
	"DROP_REPLICATION_SLOT $slotname;",
	extra_params => [ '--dbname' => $connstr_rep ]);

note "switching to physical replication slot";

# Switch to using a physical replication slot. We can do this without a new
# backup since physical slots can go backwards if needed. Do so on both
# standbys. Since we're going to be testing things that affect the slot state,
# also increase the standby feedback interval to ensure timely updates.
my ($slotname_1, $slotname_2) = ('standby_1', 'standby_2');
$node_primary->append_conf('postgresql.conf', "max_replication_slots = 4");
$node_primary->restart;
is( $node_primary->psql(
		'postgres',
		qq[SELECT pg_create_physical_replication_slot('$slotname_1');]),
	0,
	'physical slot created on primary');
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
my ($xmin, $catalog_xmin) = get_slot_xmins($node_primary, $slotname_1,
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
$node_primary->safe_psql('postgres', 'CREATE TABLE replayed(val integer);');

sub replay_check
{
	my $newval = $node_primary->safe_psql('postgres',
		'INSERT INTO replayed(val) SELECT coalesce(max(val),0) + 1 AS newval FROM replayed RETURNING val'
	);
	$node_primary->wait_for_replay_catchup($node_standby_1);
	$node_standby_1->wait_for_replay_catchup($node_standby_2, $node_primary);

	$node_standby_1->safe_psql('postgres',
		qq[SELECT 1 FROM replayed WHERE val = $newval])
	  or die "standby_1 didn't replay primary value $newval";
	$node_standby_2->safe_psql('postgres',
		qq[SELECT 1 FROM replayed WHERE val = $newval])
	  or die "standby_2 didn't replay standby_1 value $newval";
	return;
}

replay_check();

my $evttrig = $node_standby_1->safe_psql('postgres',
	"SELECT evtname FROM pg_event_trigger WHERE evtevent = 'login'");
is($evttrig, 'on_login_trigger', 'Name of login trigger');
$evttrig = $node_standby_2->safe_psql('postgres',
	"SELECT evtname FROM pg_event_trigger WHERE evtevent = 'login'");
is($evttrig, 'on_login_trigger', 'Name of login trigger');

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

($xmin, $catalog_xmin) = get_slot_xmins($node_primary, $slotname_1,
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
$node_primary->safe_psql(
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

$node_primary->safe_psql('postgres', 'VACUUM;');
$node_primary->safe_psql('postgres', 'CHECKPOINT;');

my ($xmin2, $catalog_xmin2) =
  get_slot_xmins($node_primary, $slotname_1, "xmin <> '$xmin'");
note "primary slot's new xmin $xmin2, old xmin $xmin";
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

($xmin, $catalog_xmin) = get_slot_xmins($node_primary, $slotname_1,
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
$node_standby_2->enable_streaming($node_primary);
$node_standby_2->reload;

# The WAL receiver should have generated some IO statistics.
my $stats_reads = $node_standby_1->safe_psql(
	'postgres',
	qq{SELECT sum(writes) > 0 FROM pg_stat_io
   WHERE backend_type = 'walreceiver' AND object = 'wal'});
is($stats_reads, 't', "WAL receiver generates statistics for WAL writes");

# be sure do not streaming from cascade
$node_standby_1->stop;

my $newval = $node_primary->safe_psql('postgres',
	'INSERT INTO replayed(val) SELECT coalesce(max(val),0) + 1 AS newval FROM replayed RETURNING val'
);
$node_primary->wait_for_catchup($node_standby_2);
my $is_replayed = $node_standby_2->safe_psql('postgres',
	qq[SELECT 1 FROM replayed WHERE val = $newval]);
is($is_replayed, qq(1), "standby_2 didn't replay primary value $newval");

# Drop any existing slots on the primary, for the follow-up tests.
$node_primary->safe_psql('postgres',
	"SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots;");

# Test physical slot advancing and its durability.  Create a new slot on
# the primary, not used by any of the standbys. This reserves WAL at creation.
my $phys_slot = 'phys_slot';
$node_primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('$phys_slot', true);");
# Generate some WAL, and switch to a new segment, used to check that
# the previous segment is correctly getting recycled as the slot advancing
# would recompute the minimum LSN calculated across all slots.
my $segment_removed = $node_primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');
chomp($segment_removed);
$node_primary->advance_wal(1);
my $current_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
chomp($current_lsn);
my $psql_rc = $node_primary->psql('postgres',
	"SELECT pg_replication_slot_advance('$phys_slot', '$current_lsn'::pg_lsn);"
);
is($psql_rc, '0', 'slot advancing with physical slot');
my $phys_restart_lsn_pre = $node_primary->safe_psql('postgres',
	"SELECT restart_lsn from pg_replication_slots WHERE slot_name = '$phys_slot';"
);
chomp($phys_restart_lsn_pre);
# Slot advance should persist across clean restarts.
$node_primary->restart;
my $phys_restart_lsn_post = $node_primary->safe_psql('postgres',
	"SELECT restart_lsn from pg_replication_slots WHERE slot_name = '$phys_slot';"
);
chomp($phys_restart_lsn_post);
ok( ($phys_restart_lsn_pre cmp $phys_restart_lsn_post) == 0,
	"physical slot advance persists across restarts");

# Check if the previous segment gets correctly recycled after the
# server stopped cleanly, causing a shutdown checkpoint to be generated.
my $primary_data = $node_primary->data_dir;
ok(!-f "$primary_data/pg_wal/$segment_removed",
	"WAL segment $segment_removed recycled after physical slot advancing");

note "testing pg_backup_start() followed by BASE_BACKUP";
my $connstr = $node_primary->connstr('postgres') . " replication=database";

# This test requires a replication connection with a database, as it mixes
# a replication command and a SQL command.
$node_primary->command_fails_like(
	[
		'psql',
		'--no-psqlrc',
		'--command' => "SELECT pg_backup_start('backup', true)",
		'--command' => 'BASE_BACKUP',
		'--dbname' => $connstr
	],
	qr/a backup is already in progress in this session/,
	'BASE_BACKUP cannot run in session already running backup');

note "testing BASE_BACKUP cancellation";

my $sigchld_bb_timeout =
  IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default);

# This test requires a replication connection with a database, as it mixes
# a replication command and a SQL command.  The first BASE_BACKUP is throttled
# to give enough room for the cancellation running below.  The second command
# for pg_backup_stop() should fail.
my ($sigchld_bb_stdin, $sigchld_bb_stdout, $sigchld_bb_stderr) = ('', '', '');
my $sigchld_bb = IPC::Run::start(
	[
		'psql', '--no-psqlrc',
		'--command' => "BASE_BACKUP (CHECKPOINT 'fast', MAX_RATE 32);",
		'--command' => 'SELECT pg_backup_stop()',
		'--dbname' => $connstr
	],
	'<' => \$sigchld_bb_stdin,
	'>' => \$sigchld_bb_stdout,
	'2>' => \$sigchld_bb_stderr,
	$sigchld_bb_timeout);

# The cancellation is issued once the database files are streamed and
# the checkpoint issued at backup start completes.
is( $node_primary->poll_query_until(
		'postgres',
		"SELECT pg_cancel_backend(a.pid) FROM "
		  . "pg_stat_activity a, pg_stat_progress_basebackup b WHERE "
		  . "a.pid = b.pid AND a.query ~ 'BASE_BACKUP' AND "
		  . "b.phase = 'streaming database files';"),
	"1",
	"WAL sender sending base backup killed");

# The psql command should fail on pg_backup_stop().
ok( pump_until(
		$sigchld_bb, $sigchld_bb_timeout,
		\$sigchld_bb_stderr, qr/backup is not in progress/),
	'base backup cleanly canceled');
$sigchld_bb->finish();

done_testing();
