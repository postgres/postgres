
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test suite for testing enabling data checksums in an online cluster,
# comprising of a primary and a replicated standby, with concurrent activity
# via pgbench runs

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# This test suite is expensive, or very expensive, to execute.  There are two
# PG_TEST_EXTRA options for running it, "checksum" for a pared-down test suite
# an "checksum_extended" for the full suite.  The full suite can run for hours
# on slow or constrained systems.
my $extended = undef;
if ($ENV{PG_TEST_EXTRA})
{
	$extended = 1 if ($ENV{PG_TEST_EXTRA} =~ /\bchecksum_extended\b/);
	plan skip_all => 'Expensive data checksums test disabled'
	  unless ($ENV{PG_TEST_EXTRA} =~ /\bchecksum(_extended)?\b/);
}
else
{
	plan skip_all => 'Expensive data checksums test disabled';
}

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node_primary_slot = 'physical_slot';
my $node_primary_backup = 'primary_backup';
my $node_primary;
my $node_primary_loglocation = 0;
my $node_standby;
my $node_standby_loglocation = 0;

# The number of full test iterations which will be performed. The exact number
# of tests performed and the wall time taken is non-deterministic as the test
# performs a lot of randomized actions, but 5 iterations will be a long test
# run regardless.
my $TEST_ITERATIONS = 1;
$TEST_ITERATIONS = 5 if ($extended);

# Variables which record the current state of the cluster
my $data_checksum_state = 'off';

my $pgbench_primary = undef;
my $pgbench_standby = undef;

# Start a pgbench run in the background against the server specified via the
# port passed as parameter
sub background_pgbench
{
	my ($port, $standby) = @_;
	my $pgbench = ($standby ? \$pgbench_standby : \$pgbench_primary);

	# Terminate any currently running pgbench process before continuing
	$$pgbench->finish if $$pgbench;

	my $clients = 1;
	my $runtime = 5;

	if ($extended)
	{
		# Randomize the number of pgbench clients a bit (range 1-16)
		$clients = 1 + int(rand(15));
		$runtime = 600;
	}

	my @cmd = ('pgbench', '-p', $port, '-T', $runtime, '-c', $clients);
	# Randomize whether we spawn connections or not
	push(@cmd, '-C') if ($extended && cointoss());
	# If we run on a standby it needs to be a read-only benchmark
	push(@cmd, '-S') if ($standby);
	push(@cmd, '-n') if ($standby);
	# Finally add the database name to use
	push(@cmd, 'postgres');

	$$pgbench = IPC::Run::start(
		\@cmd,
		'<' => '/dev/null',
		'>' => '/dev/null',
		'2>' => '/dev/null',
		IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default));
}

# Invert the state of data checksums in the cluster, if data checksums are on
# then disable them and vice versa. Also performs proper validation of the
# before and after state.
sub flip_data_checksums
{
	my $temptablewait = 0;

	# First, make sure the cluster is in the state we expect it to be
	test_checksum_state($node_primary, $data_checksum_state);
	test_checksum_state($node_standby, $data_checksum_state);

	if ($data_checksum_state eq 'off')
	{
		# Coin-toss to see if we are injecting a retry due to a temptable
		if (cointoss())
		{
			$node_primary->safe_psql('postgres',
				"SELECT injection_points_attach('datachecksumsworker-fake-temptable-wait', 'notice');"
			);
			$temptablewait = 1;
		}

		# log LSN right before we start changing checksums
		my $result =
		  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN before enabling: " . $result . "\n");

		# Ensure that the primary switches to "inprogress-on"
		enable_data_checksums($node_primary, wait => 'inprogress-on');

		random_sleep() if ($extended);

		# Wait for checksum enable to be replayed
		$node_primary->wait_for_catchup($node_standby, 'replay');

		# Ensure that the standby has switched to "inprogress-on" or "on".
		# Normally it would be "inprogress-on", but it is theoretically
		# possible for the primary to complete the checksum enabling *and* have
		# the standby replay that record before we reach the check below.
		$result = $node_standby->poll_query_until(
			'postgres',
			"SELECT setting = 'off' "
			  . "FROM pg_catalog.pg_settings "
			  . "WHERE name = 'data_checksums';",
			'f');
		is($result, 1,
			'ensure standby has absorbed the inprogress-on barrier');
		$result = $node_standby->safe_psql('postgres',
				"SELECT setting "
			  . "FROM pg_catalog.pg_settings "
			  . "WHERE name = 'data_checksums';");

		is( ($result eq 'inprogress-on' || $result eq 'on'),
			1,
			'ensure checksums are on, or in progress, on standby_1, got: '
			  . $result);

		# Wait for checksums enabled on the primary and standby
		wait_for_checksum_state($node_primary, 'on');

		# log LSN right after the primary flips checksums to "on"
		$result =
		  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN after enabling: " . $result . "\n");

		random_sleep() if ($extended);
		wait_for_checksum_state($node_standby, 'on');

		$node_primary->safe_psql('postgres',
			"SELECT injection_points_detach('datachecksumsworker-fake-temptable-wait');"
		) if ($temptablewait);
		$data_checksum_state = 'on';
	}
	elsif ($data_checksum_state eq 'on')
	{
		random_sleep() if ($extended);

		# log LSN right before we start changing checksums
		my $result =
		  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN before disabling: " . $result . "\n");

		disable_data_checksums($node_primary);
		$node_primary->wait_for_catchup($node_standby, 'replay');

		# Wait for checksums disabled on the primary and standby
		random_sleep() if ($extended);
		wait_for_checksum_state($node_primary, 'off');
		wait_for_checksum_state($node_standby, 'off');

		# log LSN right after the primary flips checksums to "off"
		$result =
		  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN after disabling: " . $result . "\n");

		$data_checksum_state = 'off';
	}
	else
	{
		# This should only happen due to programmer error when hacking on the
		# test code, but since that might pass subtly we error out.
		BAIL_OUT('data_checksum_state variable has invalid state:'
			  . $data_checksum_state);
	}
}

# Create and start a cluster with one primary and one standby node, and ensure
# they are caught up and in sync.
$node_primary = PostgreSQL::Test::Cluster->new('pgbench_standby_main');
$node_primary->init(allows_streaming => 1, no_data_checksums => 1);
# max_connections need to be bumped in order to accommodate for pgbench clients
# and log_statement is dialled down since it otherwise will generate enormous
# amounts of logging. Page verification failures are still logged.
$node_primary->append_conf(
	'postgresql.conf',
	qq[
max_connections = 30
log_statement = none
hot_standby_feedback = on
]);
$node_primary->start;
$node_primary->safe_psql('postgres', 'CREATE EXTENSION test_checksums;');
$node_primary->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
# Create some content to have un-checksummed data in the cluster
$node_primary->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1, 100000) AS a;");
$node_primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('$node_primary_slot');");
$node_primary->backup($node_primary_backup);

$node_standby = PostgreSQL::Test::Cluster->new('pgbench_standby_standby');
$node_standby->init_from_backup($node_primary, $node_primary_backup,
	has_streaming => 1);
$node_standby->append_conf(
	'postgresql.conf', qq[
primary_slot_name = '$node_primary_slot'
]);
$node_standby->start;

# Initialize pgbench and wait for the objects to be created on the standby
my $scalefactor = ($extended ? 10 : 1);
$node_primary->command_ok(
	[
		'pgbench', '-p', $node_primary->port, '-i', '-s', $scalefactor, '-q',
		'postgres'
	]);
$node_primary->wait_for_catchup($node_standby, 'replay');

# Start the test suite with pgbench running on all nodes
background_pgbench($node_standby->port, 1);
background_pgbench($node_primary->port, 0);

# Main test suite. This loop will start a pgbench run on the cluster and while
# that's running flip the state of data checksums concurrently. It will then
# randomly restart the cluster and then check for
# the desired state.  The idea behind doing things randomly is to stress out
# any timing related issues by subjecting the cluster for varied workloads.
# A TODO is to generate a trace such that any test failure can be traced to
# its order of operations for debugging.
for (my $i = 0; $i < $TEST_ITERATIONS; $i++)
{
	note("iteration ", ($i + 1), " of ", $TEST_ITERATIONS);

	if (!$node_primary->is_alive)
	{
		# start, to do recovery, and stop
		$node_primary->start;
		$node_primary->stop('fast');

		# Since the log isn't being written to now, parse the log and check
		# for instances of checksum verification failures.
		my $log = PostgreSQL::Test::Utils::slurp_file($node_primary->logfile,
			$node_primary_loglocation);
		unlike(
			$log,
			qr/page verification failed,.+\d$/,
			"no checksum validation errors in primary log (during WAL recovery)"
		);
		$node_primary_loglocation = -s $node_primary->logfile;

		# randomize the WAL size, to trigger checkpoints less/more often
		my $sb = 32 + int(rand(960));
		$node_primary->append_conf('postgresql.conf', qq[max_wal_size = $sb]);

		note("changing primary max_wal_size to " . $sb);

		$node_primary->start;

		# Start a pgbench in the background against the primary
		background_pgbench($node_primary->port, 0);
	}

	if (!$node_standby->is_alive)
	{
		$node_standby->start;
		$node_standby->stop('fast');

		# Since the log isn't being written to now, parse the log and check
		# for instances of checksum verification failures.
		my $log =
		  PostgreSQL::Test::Utils::slurp_file($node_standby->logfile,
			$node_standby_loglocation);
		unlike(
			$log,
			qr/page verification failed,.+\d$/,
			"no checksum validation errors in standby_1 log (during WAL recovery)"
		);
		$node_standby_loglocation = -s $node_standby->logfile;

		# randomize the WAL size, to trigger checkpoints less/more often
		my $sb = 32 + int(rand(960));
		$node_standby->append_conf('postgresql.conf', qq[max_wal_size = $sb]);

		note("changing standby max_wal_size to " . $sb);

		$node_standby->start;

		# Start a read-only pgbench in the background on the standby
		background_pgbench($node_standby->port, 1);
	}

	$node_primary->safe_psql('postgres', "UPDATE t SET a = a + 1;");
	$node_primary->wait_for_catchup($node_standby, 'write');

	flip_data_checksums();
	random_sleep() if ($extended);
	my $result = $node_primary->safe_psql('postgres',
		"SELECT count(*) FROM t WHERE a > 1");
	is($result, '100000', 'ensure data pages can be read back on primary');
	random_sleep();

	# Potentially powercycle the cluster (the nodes independently). A TODO is
	# to randomly stop the nodes in the opposite order too.
	if ($extended && cointoss())
	{
		$node_primary->stop(stopmode());

		# print the contents of the control file on the primary
		PostgreSQL::Test::Utils::system_log("pg_controldata",
			$node_primary->data_dir);

		# slurp the file after shutdown, so that it doesn't interfere with the recovery
		my $log = PostgreSQL::Test::Utils::slurp_file($node_primary->logfile,
			$node_primary_loglocation);
		unlike(
			$log,
			qr/page verification failed,.+\d$/,
			"no checksum validation errors in primary log (outside WAL recovery)"
		);
		$node_primary_loglocation = -s $node_primary->logfile;
	}

	random_sleep() if ($extended);

	if ($extended && cointoss())
	{
		$node_standby->stop(stopmode());

		# print the contents of the control file on the standby
		PostgreSQL::Test::Utils::system_log("pg_controldata",
			$node_standby->data_dir);

		# slurp the file after shutdown, so that it doesn't interfere with the recovery
		my $log =
		  PostgreSQL::Test::Utils::slurp_file($node_standby->logfile,
			$node_standby_loglocation);
		unlike(
			$log,
			qr/page verification failed,.+\d$/,
			"no checksum validation errors in standby_1 log (outside WAL recovery)"
		);
		$node_standby_loglocation = -s $node_standby->logfile;
	}
}

# make sure the nodes are running
if (!$node_primary->is_alive)
{
	$node_primary->start;
}

if (!$node_standby->is_alive)
{
	$node_standby->start;
}

# Testrun is over, ensure that data reads back as expected and perform a final
# verification of the data checksum state.
my $result =
  $node_primary->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '100000', 'ensure data pages can be read back on primary');
test_checksum_state($node_primary, $data_checksum_state);
test_checksum_state($node_standby, $data_checksum_state);

# Perform one final pass over the logs and hunt for unexpected errors
my $log = PostgreSQL::Test::Utils::slurp_file($node_primary->logfile,
	$node_primary_loglocation);
unlike(
	$log,
	qr/page verification failed,.+\d$/,
	"no checksum validation errors in primary log");
$node_primary_loglocation = -s $node_primary->logfile;
$log = PostgreSQL::Test::Utils::slurp_file($node_standby->logfile,
	$node_standby_loglocation);
unlike(
	$log,
	qr/page verification failed,.+\d$/,
	"no checksum validation errors in standby_1 log");
$node_standby_loglocation = -s $node_standby->logfile;

$node_standby->teardown_node;
$node_primary->teardown_node;

done_testing();
