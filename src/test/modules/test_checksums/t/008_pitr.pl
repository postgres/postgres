
# Copyright (c) 2026, PostgreSQL Global Development Group

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
# an "checksum_extended" for the full suite.
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


my $pgbench = undef;
my $data_checksum_state = 'off';

my $node_primary;

# Invert the state of data checksums in the cluster, if data checksums are on
# then disable them and vice versa. Also performs proper validation of the
# before and after state.
sub flip_data_checksums
{
	my $lsn_pre = undef;
	my $lsn_post = undef;

	# First, make sure the cluster is in the state we expect it to be
	test_checksum_state($node_primary, $data_checksum_state);

	if ($data_checksum_state eq 'off')
	{
		# log LSN right before we start changing checksums
		$lsn_pre =
		  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN before enabling: " . $lsn_pre . "\n");

		# Wait for checksums enabled on the primary
		enable_data_checksums($node_primary, wait => 'on');

		# log LSN right after the primary flips checksums to "on"
		$lsn_post =
		  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN after enabling: " . $lsn_post . "\n");

		$data_checksum_state = 'on';
	}
	elsif ($data_checksum_state eq 'on')
	{
		# log LSN right before we start changing checksums
		$lsn_pre =
		  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN before disabling: " . $lsn_pre . "\n");

		# Disable checksums on the primary and wait for completion
		disable_data_checksums($node_primary, wait => 1);

		# log LSN right after the primary flips checksums to "off"
		$lsn_post =
		  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");
		note("LSN after disabling: " . $lsn_post . "\n");

		$data_checksum_state = 'off';
	}
	else
	{
		# This should only happen due to programmer error when hacking on the
		# test code, but since that might pass subtly we error out.
		BAIL_OUT('data_checksum_state variable has invalid state:'
			  . $data_checksum_state);
	}

	return ($lsn_pre, $lsn_post);
}
# Start a pgbench run in the background against the server specified via the
# port passed as parameter.
sub background_rw_pgbench
{
	my $port = shift;

	# If a previous pgbench is still running, start by shutting it down.
	$pgbench->finish if $pgbench;

	# Randomize the number of pgbench clients in extended mode, else 1 client
	my $clients = ($extended ? 1 + int(rand(15)) : 1);
	my $runtime = ($extended ? 600 : 5);

	my @cmd = ('pgbench', '-p', $port, '-T', $runtime, '-c', $clients);

	# Randomize whether we spawn connections or not
	push(@cmd, '-C') if ($extended && cointoss());
	# Finally add the database name to use
	push(@cmd, 'postgres');

	$pgbench = IPC::Run::start(
		\@cmd,
		'<' => '/dev/null',
		'>' => '/dev/null',
		'2>' => '/dev/null',
		IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default));
}

# Start a primary node with WAL archiving enabled and with enough connections
# available to handle pgbench clients.
$node_primary = PostgreSQL::Test::Cluster->new('pitr_main');
$node_primary->init(
	has_archiving => 1,
	allows_streaming => 1,
	no_data_checksums => 1);
my $timeout_unit = 's';
$node_primary->append_conf(
	'postgresql.conf',
	qq[
max_connections = 100
log_statement = none
wal_sender_timeout = $PostgreSQL::Test::Utils::timeout_default$timeout_unit
wal_receiver_timeout = $PostgreSQL::Test::Utils::timeout_default$timeout_unit
]);
$node_primary->start;

# Prime the cluster with a bit of known data which we can read back to check
# for data consistency as well as page verification faults in the logfile.
$node_primary->safe_psql('postgres',
	'CREATE TABLE t AS SELECT generate_series(1, 100000) AS a;');
# Initialize and start pgbench in read/write mode against the cluster
my $scalefactor = ($extended ? 10 : 1);
$node_primary->command_ok(
	[
		'pgbench', '-p', $node_primary->port, '-i', '-s', $scalefactor, '-q',
		'postgres'
	]);
background_rw_pgbench($node_primary->port);

# Take a backup to use for PITR
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

my ($pre_lsn, $post_lsn) = flip_data_checksums();

$node_primary->safe_psql('postgres', "UPDATE t SET a = a + 1;");
$node_primary->safe_psql('postgres', "SELECT pg_create_restore_point('a');");
$node_primary->safe_psql('postgres', "UPDATE t SET a = a + 1;");
$node_primary->stop('fast');

my $node_pitr = PostgreSQL::Test::Cluster->new('pitr_backup');
$node_pitr->init_from_backup(
	$node_primary, $backup_name,
	standby => 0,
	has_restoring => 1);
$node_pitr->append_conf(
	'postgresql.conf', qq{
recovery_target_lsn = '$post_lsn'
recovery_target_action = 'promote'
recovery_target_inclusive = on
});

$node_pitr->start;

$node_pitr->poll_query_until('postgres', "SELECT pg_is_in_recovery() = 'f';")
  or die "Timed out while waiting for PITR promotion";

test_checksum_state($node_pitr, $data_checksum_state);
my $result =
  $node_pitr->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '99999', 'ensure data pages can be read back on primary');

$node_pitr->stop;

my $log = PostgreSQL::Test::Utils::slurp_file($node_pitr->logfile, 0);
unlike(
	$log,
	qr/page verification failed,.+\d$/,
	"no checksum validation errors in pitr log");

done_testing();
