
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test suite for testing enabling data checksums in an online cluster with
# concurrent activity via pgbench runs

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

my $node;
my $node_loglocation = 0;

# The number of full test iterations which will be performed. The exact number
# of tests performed and the wall time taken is non-deterministic as the test
# performs a lot of randomized actions, but 10 iterations will be a long test
# run regardless.
my $TEST_ITERATIONS = 1;
$TEST_ITERATIONS = 10 if ($extended);

# Variables which record the current state of the cluster
my $data_checksum_state = 'off';
my $pgbench = undef;

# Start a pgbench run in the background against the server specified via the
# port passed as parameter.
sub background_rw_pgbench
{
	my $port = shift;

	# If a previous pgbench is still running, start by shutting it down.
	$pgbench->finish if $pgbench;

	my $clients = 1;
	my $runtime = 2;

	if ($extended)
	{
		# Randomize the number of pgbench clients a bit (range 1-16)
		$clients = 1 + int(rand(15));
		$runtime = 600;
	}
	my @cmd = ('pgbench', '-p', $port, '-T', $runtime, '-c', $clients);

	# Randomize whether we spawn connections or not
	push(@cmd, '-C') if ($extended && cointoss);
	# Finally add the database name to use
	push(@cmd, 'postgres');

	$pgbench = IPC::Run::start(
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
	test_checksum_state($node, $data_checksum_state);

	if ($data_checksum_state eq 'off')
	{
		# Coin-toss to see if we are injecting a retry due to a temptable
		if (cointoss())
		{
			$node->safe_psql('postgres',
				"SELECT injection_points_attach('datachecksumsworker-fake-temptable-wait', 'notice');"
			);
			$temptablewait = 1;
		}

		# log LSN right before we start changing checksums
		my $result =
		  $node->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN before enabling: " . $result . "\n");

		# Ensure that the primary switches to "inprogress-on"
		enable_data_checksums($node, wait => 'inprogress-on');

		random_sleep() if ($extended);

		# Wait for checksums enabled on the primary
		wait_for_checksum_state($node, 'on');

		# log LSN right after the primary flips checksums to "on"
		$result = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN after enabling: " . $result . "\n");

		random_sleep() if ($extended);

		$node->safe_psql('postgres',
			"SELECT injection_points_detach('datachecksumsworker-fake-temptable-wait');"
		) if ($temptablewait);
		$data_checksum_state = 'on';
	}
	elsif ($data_checksum_state eq 'on')
	{
		random_sleep() if ($extended);

		# log LSN right before we start changing checksums
		my $result =
		  $node->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN before disabling: " . $result . "\n");

		disable_data_checksums($node);

		# Wait for checksums disabled on the primary
		wait_for_checksum_state($node, 'off');

		# log LSN right after the primary flips checksums to "off"
		$result = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN after disabling: " . $result . "\n");

		random_sleep() if ($extended);

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

# Create and start a cluster with one node
$node = PostgreSQL::Test::Cluster->new('pgbench_single_main');
$node->init(allows_streaming => 1, no_data_checksums => 1);
# max_connections need to be bumped in order to accommodate for pgbench clients
# and log_statement is dialled down since it otherwise will generate enormous
# amounts of logging. Page verification failures are still logged.
$node->append_conf(
	'postgresql.conf',
	qq[
max_connections = 100
log_statement = none
]);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_checksums;');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
# Create some content to have un-checksummed data in the cluster
$node->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1, 100000) AS a;");
# Initialize pgbench
my $scalefactor = ($extended ? 10 : 1);
$node->command_ok(
	[
		'pgbench', '-p', $node->port, '-i',
		'-s', $scalefactor, '-q', 'postgres'
	]);
# Start the test suite with pgbench running.
background_rw_pgbench($node->port);

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

	if (!$node->is_alive)
	{
		# Start, to do recovery, and stop
		$node->start;
		$node->stop('fast');

		# Since the log isn't being written to now, parse the log and check
		# for instances of checksum verification failures.
		my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile,
			$node_loglocation);
		unlike(
			$log,
			qr/page verification failed,.+\d$/,
			"no checksum validation errors in primary log (during WAL recovery)"
		);
		$node_loglocation = -s $node->logfile;

		# Randomize the WAL size, to trigger checkpoints less/more often
		my $sb = 64 + int(rand(1024));
		$node->append_conf('postgresql.conf', qq[max_wal_size = $sb]);
		note("changing max_wal_size to " . $sb);

		$node->start;

		# Start a pgbench in the background against the primary
		background_rw_pgbench($node->port);
	}

	$node->safe_psql('postgres', "UPDATE t SET a = a + 1;");

	flip_data_checksums();
	random_sleep() if ($extended);
	my $result =
	  $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
	is($result, '100000', 'ensure data pages can be read back on primary');

	random_sleep() if ($extended);

	# Potentially powercycle the node
	if (cointoss())
	{
		$node->stop(stopmode());

		PostgreSQL::Test::Utils::system_log("pg_controldata",
			$node->data_dir);

		my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile,
			$node_loglocation);
		unlike(
			$log,
			qr/page verification failed,.+\d$/,
			"no checksum validation errors in primary log (outside WAL recovery)"
		);
		$node_loglocation = -s $node->logfile;
	}

	random_sleep() if ($extended);
}

# Make sure the node is running
if (!$node->is_alive)
{
	$node->start;
}

# Testrun is over, ensure that data reads back as expected and perform a final
# verification of the data checksum state.
my $result =
  $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '100000', 'ensure data pages can be read back on primary');
test_checksum_state($node, $data_checksum_state);

# Perform one final pass over the logs and hunt for unexpected errors
my $log =
  PostgreSQL::Test::Utils::slurp_file($node->logfile, $node_loglocation);
unlike(
	$log,
	qr/page verification failed,.+\d$/,
	"no checksum validation errors in primary log");
$node_loglocation = -s $node->logfile;

$node->teardown_node;

done_testing();
