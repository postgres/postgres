
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Time::HiRes qw(usleep);
use Test::More;

##################################################
# Test race condition when a restart point is running during a promotion,
# checking that WAL segments are correctly removed in the restart point
# while the promotion finishes.
#
# This test relies on an injection point that causes the checkpointer to
# wait in the middle of a restart point on a standby.  The checkpointer
# is awaken to finish its restart point only once the promotion of the
# standby is completed, and the node should be able to restart properly.
##################################################

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Initialize primary node.  log_checkpoints is required as the checkpoint
# activity is monitored based on the contents of the logs.
my $node_primary = PostgreSQL::Test::Cluster->new('master');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', q[
log_checkpoints = on
restart_after_crash = on
]);
$node_primary->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node_primary->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Setup a standby.
my $node_standby = PostgreSQL::Test::Cluster->new('standby1');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;

# Dummy table for the upcoming tests.
$node_primary->safe_psql('postgres', 'checkpoint');
$node_primary->safe_psql('postgres', 'CREATE TABLE prim_tab (a int);');

# Register an injection point on the standby so as the follow-up
# restart point will wait on it.
$node_primary->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
# Wait until the extension has been created on the standby
$node_primary->wait_for_replay_catchup($node_standby);

# Note that from this point the checkpointer will wait in the middle of
# a restart point on the standby.
$node_standby->safe_psql('postgres',
	"SELECT injection_points_attach('create-restart-point', 'wait');");

# Execute a restart point on the standby, that we will now be waiting on.
# This needs to be in the background.
my $logstart = -s $node_standby->logfile;
my $psql_session =
  $node_standby->background_psql('postgres', on_error_stop => 0);
$psql_session->query_until(
	qr/starting_checkpoint/, q(
   \echo starting_checkpoint
   CHECKPOINT;
));

# Switch one WAL segment to make the previous restart point remove the
# segment once the restart point completes.
$node_primary->safe_psql('postgres', 'INSERT INTO prim_tab VALUES (1);');
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal();');
$node_primary->wait_for_replay_catchup($node_standby);

# Wait until the checkpointer is in the middle of the restart point
# processing.
$node_standby->wait_for_event('checkpointer', 'create-restart-point');

# Check the logs that the restart point has started on standby.  This is
# optional, but let's be sure.
ok( $node_standby->log_contains(
		"restartpoint starting: fast wait", $logstart),
	"restartpoint has started");

# Trigger promotion during the restart point.
$node_primary->stop;
$node_standby->promote;

# Update the start position before waking up the checkpointer!
$logstart = -s $node_standby->logfile;

# Now wake up the checkpointer.
$node_standby->safe_psql('postgres',
	"SELECT injection_points_wakeup('create-restart-point');");

# Wait until the previous restart point completes on the newly-promoted
# standby, checking the logs for that.
my $checkpoint_complete = 0;
foreach my $i (0 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	if ($node_standby->log_contains("restartpoint complete", $logstart))
	{
		$checkpoint_complete = 1;
		last;
	}
	usleep(100_000);
}
is($checkpoint_complete, 1, 'restart point has completed');

# Kill with SIGKILL, forcing all the backends to restart.
my $psql_timeout = IPC::Run::timer(3600);
my ($killme_stdin, $killme_stdout, $killme_stderr) = ('', '', '');
my $killme = IPC::Run::start(
	[
		'psql', '--no-psqlrc', '--no-align', '--tuples-only', '--quiet',
		'--set' => 'ON_ERROR_STOP=1',
		'--file' => '-',
		'--dbname' => $node_standby->connstr('postgres')
	],
	'<' => \$killme_stdin,
	'>' => \$killme_stdout,
	'2>' => \$killme_stderr,
	$psql_timeout);
$killme_stdin .= q[
SELECT pg_backend_pid();
];
$killme->pump until $killme_stdout =~ /[[:digit:]]+[\r\n]$/;
my $pid = $killme_stdout;
chomp($pid);
$killme_stdout = '';
$killme_stderr = '';

my $ret = PostgreSQL::Test::Utils::system_log('pg_ctl', 'kill', 'KILL', $pid);
is($ret, 0, 'killed process with KILL');

# Wait until the server restarts, finish consuming output.
$killme_stdin .= q[
SELECT 1;
];
ok( pump_until(
		$killme,
		$psql_timeout,
		\$killme_stderr,
		qr/server closed the connection unexpectedly|connection to server was lost|could not send data to server/m
	),
	"psql query died successfully after SIGKILL");
$killme->finish;

# Wait till server finishes restarting.
$node_standby->poll_query_until('postgres', undef, '');

# After recovery, the server should be able to start.
my $stdout;
my $stderr;
($ret, $stdout, $stderr) = $node_standby->psql('postgres', 'select 1');
is($ret, 0, "psql connect success");
is($stdout, 1, "psql select 1");

done_testing();
