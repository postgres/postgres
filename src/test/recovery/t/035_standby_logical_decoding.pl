# Copyright (c) 2023-2025, PostgreSQL Global Development Group

# logical decoding on standby : test logical decoding,
# recovery conflict and standby promotion.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my ($stdout, $stderr, $cascading_stdout, $cascading_stderr, $handle);

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
my $node_cascading_standby =
  PostgreSQL::Test::Cluster->new('cascading_standby');
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
my $default_timeout = $PostgreSQL::Test::Utils::timeout_default;
my $res;

# Name for the physical slot on primary
my $primary_slotname = 'primary_physical';
my $standby_physical_slotname = 'standby_physical';

# Fetch xmin columns from slot's pg_replication_slots row, after waiting for
# given boolean condition to be true to ensure we've reached a quiescent state.
sub wait_for_xmins
{
	my ($node, $slotname, $check_expr) = @_;

	$node->poll_query_until(
		'postgres', qq[
		SELECT $check_expr
		FROM pg_catalog.pg_replication_slots
		WHERE slot_name = '$slotname';
	]) or die "Timed out waiting for slot xmins to advance";
}

# Create the required logical slots on standby.
sub create_logical_slots
{
	my ($node, $slot_prefix) = @_;

	my $active_slot = $slot_prefix . 'activeslot';
	my $inactive_slot = $slot_prefix . 'inactiveslot';
	$node->create_logical_slot_on_standby($node_primary, qq($inactive_slot),
		'testdb');
	$node->create_logical_slot_on_standby($node_primary, qq($active_slot),
		'testdb');
}

# Drop the logical slots on standby.
sub drop_logical_slots
{
	my ($slot_prefix) = @_;
	my $active_slot = $slot_prefix . 'activeslot';
	my $inactive_slot = $slot_prefix . 'inactiveslot';

	$node_standby->psql('postgres',
		qq[SELECT pg_drop_replication_slot('$inactive_slot')]);
	$node_standby->psql('postgres',
		qq[SELECT pg_drop_replication_slot('$active_slot')]);
}

# Acquire one of the standby logical slots created by create_logical_slots().
# In case wait is true we are waiting for an active pid on the 'activeslot' slot.
# If wait is not true it means we are testing a known failure scenario.
sub make_slot_active
{
	my ($node, $slot_prefix, $wait, $to_stdout, $to_stderr) = @_;
	my $slot_user_handle;

	my $active_slot = $slot_prefix . 'activeslot';
	$slot_user_handle = IPC::Run::start(
		[
			'pg_recvlogical',
			'--dbname' => $node->connstr('testdb'),
			'--slot' => $active_slot,
			'--option' => 'include-xids=0',
			'--option' => 'skip-empty-xacts=1',
			'--file' => '-',
			'--no-loop',
			'--start',
		],
		'>' => $to_stdout,
		'2>' => $to_stderr,
		IPC::Run::timeout($default_timeout));

	if ($wait)
	{
		# make sure activeslot is in use
		$node->poll_query_until('testdb',
			qq[SELECT EXISTS (SELECT 1 FROM pg_replication_slots WHERE slot_name = '$active_slot' AND active_pid IS NOT NULL)]
		) or die "slot never became active";
	}
	return $slot_user_handle;
}

# Check pg_recvlogical stderr
sub check_pg_recvlogical_stderr
{
	my ($slot_user_handle, $check_stderr) = @_;
	my $return;

	# our client should've terminated in response to the walsender error
	$slot_user_handle->finish;
	$return = $?;
	cmp_ok($return, "!=", 0, "pg_recvlogical exited non-zero");
	if ($return)
	{
		like($stderr, qr/$check_stderr/, 'slot has been invalidated');
	}

	return 0;
}

# Check if all the slots on standby are dropped. These include the 'activeslot'
# that was acquired by make_slot_active(), and the non-active 'inactiveslot'.
sub check_slots_dropped
{
	my ($slot_prefix, $slot_user_handle) = @_;

	is($node_standby->slot($slot_prefix . 'inactiveslot')->{'slot_type'},
		'', 'inactiveslot on standby dropped');
	is($node_standby->slot($slot_prefix . 'activeslot')->{'slot_type'},
		'', 'activeslot on standby dropped');

	check_pg_recvlogical_stderr($slot_user_handle, "conflict with recovery");
}

# Change hot_standby_feedback and check xmin and catalog_xmin values.
sub change_hot_standby_feedback_and_wait_for_xmins
{
	my ($hsf, $invalidated) = @_;

	$node_standby->append_conf(
		'postgresql.conf', qq[
	hot_standby_feedback = $hsf
	]);

	$node_standby->reload;

	if ($hsf && $invalidated)
	{
		# With hot_standby_feedback on, xmin should advance,
		# but catalog_xmin should still remain NULL since there is no logical slot.
		wait_for_xmins($node_primary, $primary_slotname,
			"xmin IS NOT NULL AND catalog_xmin IS NULL");
	}
	elsif ($hsf)
	{
		# With hot_standby_feedback on, xmin and catalog_xmin should advance.
		wait_for_xmins($node_primary, $primary_slotname,
			"xmin IS NOT NULL AND catalog_xmin IS NOT NULL");
	}
	else
	{
		# Both should be NULL since hs_feedback is off
		wait_for_xmins($node_primary, $primary_slotname,
			"xmin IS NULL AND catalog_xmin IS NULL");

	}
}

# Check reason for conflict in pg_replication_slots.
sub check_slots_conflict_reason
{
	my ($slot_prefix, $reason) = @_;

	my $active_slot = $slot_prefix . 'activeslot';
	my $inactive_slot = $slot_prefix . 'inactiveslot';

	$res = $node_standby->safe_psql(
		'postgres', qq(
			 select invalidation_reason from pg_replication_slots where slot_name = '$active_slot' and conflicting;)
	);

	is($res, "$reason", "$active_slot reason for conflict is $reason");

	$res = $node_standby->safe_psql(
		'postgres', qq(
			 select invalidation_reason from pg_replication_slots where slot_name = '$inactive_slot' and conflicting;)
	);

	is($res, "$reason", "$inactive_slot reason for conflict is $reason");
}

# Drop the slots, re-create them, change hot_standby_feedback,
# check xmin and catalog_xmin values, make slot active and reset stat.
sub reactive_slots_change_hfs_and_wait_for_xmins
{
	my ($previous_slot_prefix, $slot_prefix, $hsf, $invalidated) = @_;

	# drop the logical slots
	drop_logical_slots($previous_slot_prefix);

	# create the logical slots
	create_logical_slots($node_standby, $slot_prefix);

	change_hot_standby_feedback_and_wait_for_xmins($hsf, $invalidated);

	$handle =
	  make_slot_active($node_standby, $slot_prefix, 1, \$stdout, \$stderr);

	# reset stat: easier to check for confl_active_logicalslot in pg_stat_database_conflicts
	$node_standby->psql('testdb', q[select pg_stat_reset();]);
}

# Check invalidation in the logfile and in pg_stat_database_conflicts
sub check_for_invalidation
{
	my ($slot_prefix, $log_start, $test_name) = @_;

	my $active_slot = $slot_prefix . 'activeslot';
	my $inactive_slot = $slot_prefix . 'inactiveslot';

	# message should be issued
	ok( $node_standby->log_contains(
			"invalidating obsolete replication slot \"$inactive_slot\"",
			$log_start),
		"inactiveslot slot invalidation is logged $test_name");

	ok( $node_standby->log_contains(
			"invalidating obsolete replication slot \"$active_slot\"",
			$log_start),
		"activeslot slot invalidation is logged $test_name");

	# Verify that pg_stat_database_conflicts.confl_active_logicalslot has been updated
	ok( $node_standby->poll_query_until(
			'postgres',
			"select (confl_active_logicalslot = 1) from pg_stat_database_conflicts where datname = 'testdb'",
			't'),
		'confl_active_logicalslot updated'
	) or die "Timed out waiting confl_active_logicalslot to be updated";
}

# Launch $sql query, wait for a new snapshot that has a newer horizon and
# launch a VACUUM.  $vac_option is the set of options to be passed to the
# VACUUM command, $sql the sql to launch before triggering the vacuum and
# $to_vac the relation to vacuum.
#
# Note that the injection_point avoids seeing a xl_running_xacts that could
# advance an active replication slot's catalog_xmin. Advancing the active
# replication slot's catalog_xmin would break some tests that expect the
# active slot to conflict with the catalog xmin horizon.
sub wait_until_vacuum_can_remove
{
	my ($vac_option, $sql, $to_vac) = @_;

	# Note that from this point the checkpointer and bgwriter will skip writing
	# xl_running_xacts record.
	$node_primary->safe_psql('testdb',
		"SELECT injection_points_attach('skip-log-running-xacts', 'error');");

	# Get the current xid horizon,
	my $xid_horizon = $node_primary->safe_psql('testdb',
		qq[select pg_snapshot_xmin(pg_current_snapshot());]);

	# Launch our sql.
	$node_primary->safe_psql('testdb', qq[$sql]);

	# Wait until we get a newer horizon.
	$node_primary->poll_query_until('testdb',
		"SELECT (select pg_snapshot_xmin(pg_current_snapshot())::text::int - $xid_horizon) > 0"
	) or die "new snapshot does not have a newer horizon";

	# Launch the vacuum command and insert into flush_wal (see CREATE
	# TABLE flush_wal for the reason).
	$node_primary->safe_psql(
		'testdb', qq[VACUUM $vac_option verbose $to_vac;
										  INSERT INTO flush_wal DEFAULT VALUES;]);

	$node_primary->wait_for_replay_catchup($node_standby);

	# Resume generating the xl_running_xacts record
	$node_primary->safe_psql('testdb',
		"SELECT injection_points_detach('skip-log-running-xacts');");
}

########################
# Initialize primary node
########################

$node_primary->init(allows_streaming => 1, has_archiving => 1);
$node_primary->append_conf(
	'postgresql.conf', q{
wal_level = 'logical'
max_replication_slots = 4
max_wal_senders = 4
autovacuum = off
});
$node_primary->dump_info;
$node_primary->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node_primary->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node_primary->psql('postgres', q[CREATE DATABASE testdb]);

$node_primary->safe_psql('testdb',
	qq[SELECT * FROM pg_create_physical_replication_slot('$primary_slotname');]
);

# Check conflicting is NULL for physical slot
$res = $node_primary->safe_psql(
	'postgres', qq[
		 SELECT conflicting is null FROM pg_replication_slots where slot_name = '$primary_slotname';]
);

is($res, 't', "Physical slot reports conflicting as NULL");

my $backup_name = 'b1';
$node_primary->backup($backup_name);

# Some tests need to wait for VACUUM to be replayed. But vacuum does not flush
# WAL. An insert into flush_wal outside transaction does guarantee a flush.
$node_primary->psql('testdb', q[CREATE TABLE flush_wal();]);

#######################
# Initialize standby node
#######################

$node_standby->init_from_backup(
	$node_primary, $backup_name,
	has_streaming => 1,
	has_restoring => 1);
$node_standby->append_conf(
	'postgresql.conf',
	qq[primary_slot_name = '$primary_slotname'
       max_replication_slots = 5]);
$node_standby->start;
$node_primary->wait_for_replay_catchup($node_standby);

#######################
# Initialize subscriber node
#######################
$node_subscriber->init;
$node_subscriber->start;

my %psql_subscriber = (
	'subscriber_stdin' => '',
	'subscriber_stdout' => '',
	'subscriber_stderr' => '');
$psql_subscriber{run} = IPC::Run::start(
	[
		'psql', '--no-psqlrc', '--no-align',
		'--file' => '-',
		'--dbname' => $node_subscriber->connstr('postgres')
	],
	'<' => \$psql_subscriber{subscriber_stdin},
	'>' => \$psql_subscriber{subscriber_stdout},
	'2>' => \$psql_subscriber{subscriber_stderr},
	IPC::Run::timeout($default_timeout));

##################################################
# Test that the standby requires hot_standby to be
# enabled for pre-existing logical slots.
##################################################

# create the logical slots
$node_standby->create_logical_slot_on_standby($node_primary, 'restart_test');
$node_standby->stop;
$node_standby->append_conf('postgresql.conf', qq[hot_standby = off]);

# Use run_log instead of $node_standby->start because this test expects
# that the server ends with an error during startup.
run_log(
	[
		'pg_ctl',
		'--pgdata' => $node_standby->data_dir,
		'--log' => $node_standby->logfile,
		'start',
	]);

# wait for postgres to terminate
foreach my $i (0 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	last if !-f $node_standby->data_dir . '/postmaster.pid';
	usleep(100_000);
}

# Confirm that the server startup fails with an expected error
my $logfile = slurp_file($node_standby->logfile());
ok( $logfile =~
	  qr/FATAL: .* logical replication slot ".*" exists on the standby, but "hot_standby" = "off"/,
	"the standby ends with an error during startup because hot_standby was disabled"
);
$node_standby->adjust_conf('postgresql.conf', 'hot_standby', 'on');
$node_standby->start;
$node_standby->safe_psql('postgres',
	qq[SELECT pg_drop_replication_slot('restart_test')]);

##################################################
# Test that logical decoding on the standby
# behaves correctly.
##################################################

# create the logical slots
create_logical_slots($node_standby, 'behaves_ok_');

$node_primary->safe_psql('testdb',
	qq[CREATE TABLE decoding_test(x integer, y text);]);
$node_primary->safe_psql('testdb',
	qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,10) s;]
);

$node_primary->wait_for_replay_catchup($node_standby);

my $result = $node_standby->safe_psql('testdb',
	qq[SELECT pg_logical_slot_get_changes('behaves_ok_activeslot', NULL, NULL);]
);

# test if basic decoding works
is(scalar(my @foobar = split /^/m, $result),
	14, 'Decoding produced 14 rows (2 BEGIN/COMMIT and 10 rows)');

# Insert some rows and verify that we get the same results from pg_recvlogical
# and the SQL interface.
$node_primary->safe_psql('testdb',
	qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,4) s;]
);

my $expected = q{BEGIN
table public.decoding_test: INSERT: x[integer]:1 y[text]:'1'
table public.decoding_test: INSERT: x[integer]:2 y[text]:'2'
table public.decoding_test: INSERT: x[integer]:3 y[text]:'3'
table public.decoding_test: INSERT: x[integer]:4 y[text]:'4'
COMMIT};

$node_primary->wait_for_replay_catchup($node_standby);

my $stdout_sql = $node_standby->safe_psql('testdb',
	qq[SELECT data FROM pg_logical_slot_peek_changes('behaves_ok_activeslot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');]
);

is($stdout_sql, $expected, 'got expected output from SQL decoding session');

my $endpos = $node_standby->safe_psql('testdb',
	"SELECT lsn FROM pg_logical_slot_peek_changes('behaves_ok_activeslot', NULL, NULL) ORDER BY lsn DESC LIMIT 1;"
);

# Insert some rows after $endpos, which we won't read.
$node_primary->safe_psql('testdb',
	qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(5,50) s;]
);

$node_primary->wait_for_replay_catchup($node_standby);

my $stdout_recv = $node_standby->pg_recvlogical_upto(
	'testdb', 'behaves_ok_activeslot', $endpos, $default_timeout,
	'include-xids' => '0',
	'skip-empty-xacts' => '1');
chomp($stdout_recv);
is($stdout_recv, $expected,
	'got same expected output from pg_recvlogical decoding session');

$node_standby->poll_query_until('testdb',
	"SELECT EXISTS (SELECT 1 FROM pg_replication_slots WHERE slot_name = 'behaves_ok_activeslot' AND active_pid IS NULL)"
) or die "slot never became inactive";

$stdout_recv = $node_standby->pg_recvlogical_upto(
	'testdb', 'behaves_ok_activeslot', $endpos, $default_timeout,
	'include-xids' => '0',
	'skip-empty-xacts' => '1');
chomp($stdout_recv);
is($stdout_recv, '', 'pg_recvlogical acknowledged changes');

$node_primary->safe_psql('postgres', 'CREATE DATABASE otherdb');

# Wait for catchup to ensure that the new database is visible to other sessions
# on the standby.
$node_primary->wait_for_replay_catchup($node_standby);

($result, $stdout, $stderr) = $node_standby->psql('otherdb',
	"SELECT lsn FROM pg_logical_slot_peek_changes('behaves_ok_activeslot', NULL, NULL) ORDER BY lsn DESC LIMIT 1;"
);
ok( $stderr =~
	  m/replication slot "behaves_ok_activeslot" was not created in this database/,
	"replaying logical slot from another database fails");

##################################################
# Test that we can subscribe on the standby with the publication
# created on the primary.
##################################################

# Create a table on the primary
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_rep (a int primary key)");

# Create a table (same structure) on the subscriber node
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rep (a int primary key)");

# Create a publication on the primary
$node_primary->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub for table tab_rep");

$node_primary->wait_for_replay_catchup($node_standby);

# Subscribe on the standby
my $standby_connstr = $node_standby->connstr . ' dbname=postgres';

# Not using safe_psql() here as it would wait for activity on the primary
# and we wouldn't be able to launch pg_log_standby_snapshot() on the primary
# while waiting.
# psql_subscriber() allows to not wait synchronously.
$psql_subscriber{subscriber_stdin} .= qq[CREATE SUBSCRIPTION tap_sub
     CONNECTION '$standby_connstr'
     PUBLICATION tap_pub
     WITH (copy_data = off);];
$psql_subscriber{subscriber_stdin} .= "\n";

$psql_subscriber{run}->pump_nb();

# Log the standby snapshot to speed up the subscription creation
$node_primary->log_standby_snapshot($node_standby, 'tap_sub');

# Explicitly shut down psql instance gracefully - to avoid hangs
# or worse on windows
$psql_subscriber{subscriber_stdin} .= "\\q\n";
$psql_subscriber{run}->finish;

$node_subscriber->wait_for_subscription_sync($node_standby, 'tap_sub');

# Insert some rows on the primary
$node_primary->safe_psql('postgres',
	qq[INSERT INTO tab_rep select generate_series(1,10);]);

$node_primary->wait_for_replay_catchup($node_standby);
$node_standby->wait_for_catchup('tap_sub');

# Check that the subscriber can see the rows inserted in the primary
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab_rep");
is($result, qq(10), 'check replicated inserts after subscription on standby');

# We do not need the subscription and the subscriber anymore
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");
$node_subscriber->stop;

# Create the injection_points extension
$node_primary->safe_psql('testdb', 'CREATE EXTENSION injection_points;');

##################################################
# Recovery conflict: Invalidate conflicting slots, including in-use slots
# Scenario 1: hot_standby_feedback off and vacuum FULL
#
# In passing, ensure that replication slot stats are not removed when the
# active slot is invalidated, and check that an error occurs when
# attempting to alter the invalid slot.
##################################################

# One way to produce recovery conflict is to create/drop a relation and
# launch a vacuum full on pg_class with hot_standby_feedback turned off on
# the standby.
reactive_slots_change_hfs_and_wait_for_xmins('behaves_ok_', 'vacuum_full_',
	0, 1);

# Ensure that replication slot stats are not empty before triggering the
# conflict.
$node_primary->safe_psql('testdb',
	qq[INSERT INTO decoding_test(x,y) SELECT 100,'100';]);

$node_standby->poll_query_until('testdb',
	qq[SELECT total_txns > 0 FROM pg_stat_replication_slots WHERE slot_name = 'vacuum_full_activeslot']
) or die "replication slot stats of vacuum_full_activeslot not updated";

# This should trigger the conflict
wait_until_vacuum_can_remove(
	'full', 'CREATE TABLE conflict_test(x integer, y text);
								 DROP TABLE conflict_test;', 'pg_class');

# Check invalidation in the logfile and in pg_stat_database_conflicts
check_for_invalidation('vacuum_full_', 1, 'with vacuum FULL on pg_class');

# Verify reason for conflict is 'rows_removed' in pg_replication_slots
check_slots_conflict_reason('vacuum_full_', 'rows_removed');

# Attempting to alter an invalidated slot should result in an error
($result, $stdout, $stderr) = $node_standby->psql(
	'postgres',
	qq[ALTER_REPLICATION_SLOT vacuum_full_inactiveslot (failover);],
	replication => 'database');
ok( $stderr =~
	  /ERROR:  can no longer access replication slot "vacuum_full_inactiveslot"/
	  && $stderr =~
	  /DETAIL:  This replication slot has been invalidated due to "rows_removed"./,
	"invalidated slot cannot be altered");

# Ensure that replication slot stats are not removed after invalidation.
is( $node_standby->safe_psql(
		'testdb',
		qq[SELECT total_txns > 0 FROM pg_stat_replication_slots WHERE slot_name = 'vacuum_full_activeslot']
	),
	't',
	'replication slot stats not removed after invalidation');

$handle =
  make_slot_active($node_standby, 'vacuum_full_', 0, \$stdout, \$stderr);

# We are not able to read from the slot as it has been invalidated
check_pg_recvlogical_stderr($handle,
	"can no longer access replication slot \"vacuum_full_activeslot\"");

# Attempt to copy an invalidated logical replication slot
($result, $stdout, $stderr) = $node_standby->psql(
	'postgres',
	qq[select pg_copy_logical_replication_slot('vacuum_full_inactiveslot', 'vacuum_full_inactiveslot_copy');],
	replication => 'database');
ok( $stderr =~
	  /ERROR:  cannot copy invalidated replication slot "vacuum_full_inactiveslot"/,
	"invalidated slot cannot be copied");

# Turn hot_standby_feedback back on
change_hot_standby_feedback_and_wait_for_xmins(1, 1);

##################################################
# Verify that invalidated logical slots stay invalidated across a restart.
##################################################
$node_standby->restart;

# Verify reason for conflict is retained across a restart.
check_slots_conflict_reason('vacuum_full_', 'rows_removed');

##################################################
# Verify that invalidated logical slots do not lead to retaining WAL.
##################################################

# Get the restart_lsn from an invalidated slot
my $restart_lsn = $node_standby->safe_psql(
	'postgres',
	"SELECT restart_lsn FROM pg_replication_slots
		WHERE slot_name = 'vacuum_full_activeslot' AND conflicting;"
);

chomp($restart_lsn);

# As pg_walfile_name() can not be executed on the standby,
# get the WAL file name associated to this lsn from the primary
my $walfile_name = $node_primary->safe_psql('postgres',
	"SELECT pg_walfile_name('$restart_lsn')");

chomp($walfile_name);

# Generate some activity and switch WAL file on the primary
$node_primary->advance_wal(1);
$node_primary->safe_psql('postgres', "checkpoint;");

# Wait for the standby to catch up
$node_primary->wait_for_replay_catchup($node_standby);

# Request a checkpoint on the standby to trigger the WAL file(s) removal
$node_standby->safe_psql('postgres', 'checkpoint;');

# Verify that the WAL file has not been retained on the standby
my $standby_walfile = $node_standby->data_dir . '/pg_wal/' . $walfile_name;
ok(!-f "$standby_walfile",
	"invalidated logical slots do not lead to retaining WAL");

##################################################
# Recovery conflict: Invalidate conflicting slots, including in-use slots
# Scenario 2: conflict due to row removal with hot_standby_feedback off.
##################################################

# get the position to search from in the standby logfile
my $logstart = -s $node_standby->logfile;

# One way to produce recovery conflict is to create/drop a relation and
# launch a vacuum on pg_class with hot_standby_feedback turned off on the
# standby.
reactive_slots_change_hfs_and_wait_for_xmins('vacuum_full_', 'row_removal_',
	0, 1);

# This should trigger the conflict
wait_until_vacuum_can_remove(
	'', 'CREATE TABLE conflict_test(x integer, y text);
							 DROP TABLE conflict_test;', 'pg_class');

# Check invalidation in the logfile and in pg_stat_database_conflicts
check_for_invalidation('row_removal_', $logstart, 'with vacuum on pg_class');

# Verify reason for conflict is 'rows_removed' in pg_replication_slots
check_slots_conflict_reason('row_removal_', 'rows_removed');

$handle =
  make_slot_active($node_standby, 'row_removal_', 0, \$stdout, \$stderr);

# We are not able to read from the slot as it has been invalidated
check_pg_recvlogical_stderr($handle,
	"can no longer access replication slot \"row_removal_activeslot\"");

##################################################
# Recovery conflict: Same as Scenario 2 but on a shared catalog table
# Scenario 3: conflict due to row removal with hot_standby_feedback off.
##################################################

# get the position to search from in the standby logfile
$logstart = -s $node_standby->logfile;

# One way to produce recovery conflict on a shared catalog table is to
# create/drop a role and launch a vacuum on pg_authid with
# hot_standby_feedback turned off on the standby.
reactive_slots_change_hfs_and_wait_for_xmins('row_removal_',
	'shared_row_removal_', 0, 1);

# Trigger the conflict
wait_until_vacuum_can_remove(
	'', 'CREATE ROLE create_trash;
							 DROP ROLE create_trash;', 'pg_authid');

# Check invalidation in the logfile and in pg_stat_database_conflicts
check_for_invalidation('shared_row_removal_', $logstart,
	'with vacuum on pg_authid');

# Verify reason for conflict is 'rows_removed' in pg_replication_slots
check_slots_conflict_reason('shared_row_removal_', 'rows_removed');

$handle = make_slot_active($node_standby, 'shared_row_removal_', 0, \$stdout,
	\$stderr);

# We are not able to read from the slot as it has been invalidated
check_pg_recvlogical_stderr($handle,
	"can no longer access replication slot \"shared_row_removal_activeslot\""
);

##################################################
# Recovery conflict: Same as Scenario 2 but on a non catalog table
# Scenario 4: No conflict expected.
##################################################

# get the position to search from in the standby logfile
$logstart = -s $node_standby->logfile;

reactive_slots_change_hfs_and_wait_for_xmins('shared_row_removal_',
	'no_conflict_', 0, 1);

# This should not trigger a conflict
wait_until_vacuum_can_remove(
	'', 'CREATE TABLE conflict_test(x integer, y text);
							 INSERT INTO conflict_test(x,y) SELECT s, s::text FROM generate_series(1,4) s;
							 UPDATE conflict_test set x=1, y=1;', 'conflict_test');

# message should not be issued
ok( !$node_standby->log_contains(
		"invalidating obsolete slot \"no_conflict_inactiveslot\"", $logstart),
	'inactiveslot slot invalidation is not logged with vacuum on conflict_test'
);

ok( !$node_standby->log_contains(
		"invalidating obsolete slot \"no_conflict_activeslot\"", $logstart),
	'activeslot slot invalidation is not logged with vacuum on conflict_test'
);

# Verify that pg_stat_database_conflicts.confl_active_logicalslot has not been updated
ok( $node_standby->poll_query_until(
		'postgres',
		"select (confl_active_logicalslot = 0) from pg_stat_database_conflicts where datname = 'testdb'",
		't'),
	'confl_active_logicalslot not updated'
) or die "Timed out waiting confl_active_logicalslot to be updated";

# Verify slots are reported as non conflicting in pg_replication_slots
is( $node_standby->safe_psql(
		'postgres',
		q[select bool_or(conflicting) from
		  (select conflicting from pg_replication_slots
			where slot_type = 'logical')]),
	'f',
	'Logical slots are reported as non conflicting');

# Turn hot_standby_feedback back on
change_hot_standby_feedback_and_wait_for_xmins(1, 0);

# Restart the standby node to ensure no slots are still active
$node_standby->restart;

##################################################
# Recovery conflict: Invalidate conflicting slots, including in-use slots
# Scenario 5: conflict due to on-access pruning.
##################################################

# get the position to search from in the standby logfile
$logstart = -s $node_standby->logfile;

# One way to produce recovery conflict is to trigger an on-access pruning
# on a relation marked as user_catalog_table.
reactive_slots_change_hfs_and_wait_for_xmins('no_conflict_', 'pruning_', 0,
	0);

# Injection point avoids seeing a xl_running_xacts. This is required because if
# it is generated between the last two updates, then the catalog_xmin of the
# active slot could be updated, and hence, the conflict won't occur. See
# comments atop wait_until_vacuum_can_remove.
$node_primary->safe_psql('testdb',
	"SELECT injection_points_attach('skip-log-running-xacts', 'error');");

# This should trigger the conflict
$node_primary->safe_psql('testdb',
	qq[CREATE TABLE prun(id integer, s char(2000)) WITH (fillfactor = 75, user_catalog_table = true);]
);
$node_primary->safe_psql('testdb', qq[INSERT INTO prun VALUES (1, 'A');]);
$node_primary->safe_psql('testdb', qq[UPDATE prun SET s = 'B';]);
$node_primary->safe_psql('testdb', qq[UPDATE prun SET s = 'C';]);
$node_primary->safe_psql('testdb', qq[UPDATE prun SET s = 'D';]);
$node_primary->safe_psql('testdb', qq[UPDATE prun SET s = 'E';]);

$node_primary->wait_for_replay_catchup($node_standby);

# Resume generating the xl_running_xacts record
$node_primary->safe_psql('testdb',
	"SELECT injection_points_detach('skip-log-running-xacts');");

# Check invalidation in the logfile and in pg_stat_database_conflicts
check_for_invalidation('pruning_', $logstart, 'with on-access pruning');

# Verify reason for conflict is 'rows_removed' in pg_replication_slots
check_slots_conflict_reason('pruning_', 'rows_removed');

$handle = make_slot_active($node_standby, 'pruning_', 0, \$stdout, \$stderr);

# We are not able to read from the slot as it has been invalidated
check_pg_recvlogical_stderr($handle,
	"can no longer access replication slot \"pruning_activeslot\"");

# Turn hot_standby_feedback back on
change_hot_standby_feedback_and_wait_for_xmins(1, 1);

##################################################
# Recovery conflict: Invalidate conflicting slots, including in-use slots
# Scenario 6: incorrect wal_level on primary.
##################################################

# get the position to search from in the standby logfile
$logstart = -s $node_standby->logfile;

# drop the logical slots
drop_logical_slots('pruning_');

# create the logical slots
create_logical_slots($node_standby, 'wal_level_');

$handle =
  make_slot_active($node_standby, 'wal_level_', 1, \$stdout, \$stderr);

# reset stat: easier to check for confl_active_logicalslot in pg_stat_database_conflicts
$node_standby->psql('testdb', q[select pg_stat_reset();]);

# Make primary wal_level replica. This will trigger slot conflict.
$node_primary->append_conf(
	'postgresql.conf', q[
wal_level = 'replica'
]);
$node_primary->restart;

$node_primary->wait_for_replay_catchup($node_standby);

# Check invalidation in the logfile and in pg_stat_database_conflicts
check_for_invalidation('wal_level_', $logstart, 'due to wal_level');

# Verify reason for conflict is 'wal_level_insufficient' in pg_replication_slots
check_slots_conflict_reason('wal_level_', 'wal_level_insufficient');

$handle =
  make_slot_active($node_standby, 'wal_level_', 0, \$stdout, \$stderr);
# We are not able to read from the slot as it requires wal_level >= logical on the primary server
check_pg_recvlogical_stderr($handle,
	"logical decoding on standby requires \"wal_level\" >= \"logical\" on the primary"
);

# Restore primary wal_level
$node_primary->append_conf(
	'postgresql.conf', q[
wal_level = 'logical'
]);
$node_primary->restart;
$node_primary->wait_for_replay_catchup($node_standby);

$handle =
  make_slot_active($node_standby, 'wal_level_', 0, \$stdout, \$stderr);
# as the slot has been invalidated we should not be able to read
check_pg_recvlogical_stderr($handle,
	"can no longer access replication slot \"wal_level_activeslot\"");

##################################################
# DROP DATABASE should drop its slots, including active slots.
##################################################

# drop the logical slots
drop_logical_slots('wal_level_');

# create the logical slots
create_logical_slots($node_standby, 'drop_db_');

$handle = make_slot_active($node_standby, 'drop_db_', 1, \$stdout, \$stderr);

# Create a slot on a database that would not be dropped. This slot should not
# get dropped.
$node_standby->create_logical_slot_on_standby($node_primary, 'otherslot',
	'postgres');

# dropdb on the primary to verify slots are dropped on standby
$node_primary->safe_psql('postgres', q[DROP DATABASE testdb]);

$node_primary->wait_for_replay_catchup($node_standby);

is( $node_standby->safe_psql(
		'postgres',
		q[SELECT EXISTS(SELECT 1 FROM pg_database WHERE datname = 'testdb')]),
	'f',
	'database dropped on standby');

check_slots_dropped('drop_db', $handle);

is($node_standby->slot('otherslot')->{'slot_type'},
	'logical', 'otherslot on standby not dropped');

# Cleanup : manually drop the slot that was not dropped.
$node_standby->psql('postgres',
	q[SELECT pg_drop_replication_slot('otherslot')]);

##################################################
# Test standby promotion and logical decoding behavior
# after the standby gets promoted.
##################################################

$node_standby->reload;

$node_primary->psql('postgres', q[CREATE DATABASE testdb]);
$node_primary->safe_psql('testdb',
	qq[CREATE TABLE decoding_test(x integer, y text);]);

# Wait for the standby to catchup before initializing the cascading standby
$node_primary->wait_for_replay_catchup($node_standby);

# Create a physical replication slot on the standby.
# Keep this step after the "Verify that invalidated logical slots do not lead
# to retaining WAL" test (as the physical slot on the standby could prevent the
# WAL file removal).
$node_standby->safe_psql('testdb',
	qq[SELECT * FROM pg_create_physical_replication_slot('$standby_physical_slotname');]
);

# Initialize cascading standby node
$node_standby->backup($backup_name);
$node_cascading_standby->init_from_backup(
	$node_standby, $backup_name,
	has_streaming => 1,
	has_restoring => 1);
$node_cascading_standby->append_conf(
	'postgresql.conf',
	qq[primary_slot_name = '$standby_physical_slotname'
	   hot_standby_feedback = on]);
$node_cascading_standby->start;

# create the logical slots
create_logical_slots($node_standby, 'promotion_');

# Wait for the cascading standby to catchup before creating the slots
$node_standby->wait_for_replay_catchup($node_cascading_standby,
	$node_primary);

# create the logical slots on the cascading standby too
create_logical_slots($node_cascading_standby, 'promotion_');

# Make slots actives
$handle =
  make_slot_active($node_standby, 'promotion_', 1, \$stdout, \$stderr);
my $cascading_handle =
  make_slot_active($node_cascading_standby, 'promotion_', 1,
	\$cascading_stdout, \$cascading_stderr);

# Insert some rows before the promotion
$node_primary->safe_psql('testdb',
	qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,4) s;]
);

# Wait for both standbys to catchup
$node_primary->wait_for_replay_catchup($node_standby);
$node_standby->wait_for_replay_catchup($node_cascading_standby,
	$node_primary);

# promote
$node_standby->promote;

# insert some rows on promoted standby
$node_standby->safe_psql('testdb',
	qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(5,7) s;]
);

# Wait for the cascading standby to catchup
$node_standby->wait_for_replay_catchup($node_cascading_standby);

$expected = q{BEGIN
table public.decoding_test: INSERT: x[integer]:1 y[text]:'1'
table public.decoding_test: INSERT: x[integer]:2 y[text]:'2'
table public.decoding_test: INSERT: x[integer]:3 y[text]:'3'
table public.decoding_test: INSERT: x[integer]:4 y[text]:'4'
COMMIT
BEGIN
table public.decoding_test: INSERT: x[integer]:5 y[text]:'5'
table public.decoding_test: INSERT: x[integer]:6 y[text]:'6'
table public.decoding_test: INSERT: x[integer]:7 y[text]:'7'
COMMIT};

# check that we are decoding pre and post promotion inserted rows
$stdout_sql = $node_standby->safe_psql('testdb',
	qq[SELECT data FROM pg_logical_slot_peek_changes('promotion_inactiveslot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');]
);

is($stdout_sql, $expected,
	'got expected output from SQL decoding session on promoted standby');

# check that we are decoding pre and post promotion inserted rows
# with pg_recvlogical that has started before the promotion
my $pump_timeout = IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default);

ok(pump_until($handle, $pump_timeout, \$stdout, qr/^.*COMMIT.*COMMIT$/s),
	'got 2 COMMIT from pg_recvlogical output');

chomp($stdout);
is($stdout, $expected,
	'got same expected output from pg_recvlogical decoding session');

# check that we are decoding pre and post promotion inserted rows on the cascading standby
$stdout_sql = $node_cascading_standby->safe_psql('testdb',
	qq[SELECT data FROM pg_logical_slot_peek_changes('promotion_inactiveslot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');]
);

is($stdout_sql, $expected,
	'got expected output from SQL decoding session on cascading standby');

# check that we are decoding pre and post promotion inserted rows
# with pg_recvlogical that has started before the promotion on the cascading standby
ok( pump_until(
		$cascading_handle, $pump_timeout,
		\$cascading_stdout, qr/^.*COMMIT.*COMMIT$/s),
	'got 2 COMMIT from pg_recvlogical output');

chomp($cascading_stdout);
is($cascading_stdout, $expected,
	'got same expected output from pg_recvlogical decoding session on cascading standby'
);

done_testing();
