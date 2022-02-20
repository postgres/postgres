package RewindTest;

# Test driver for pg_rewind. Each test consists of a cycle where a new cluster
# is first created with initdb, and a streaming replication standby is set up
# to follow the master. Then the master is shut down and the standby is
# promoted, and finally pg_rewind is used to rewind the old master, using the
# standby as the source.
#
# To run a test, the test script (in t/ subdirectory) calls the functions
# in this module. These functions should be called in this sequence:
#
# 1. setup_cluster - creates a PostgreSQL cluster that runs as the master
#
# 2. start_master - starts the master server
#
# 3. create_standby - runs pg_basebackup to initialize a standby server, and
#    sets it up to follow the master.
#
# 4. promote_standby - runs "pg_ctl promote" to promote the standby server.
# The old master keeps running.
#
# 5. run_pg_rewind - stops the old master (if it's still running) and runs
# pg_rewind to synchronize it with the now-promoted standby server.
#
# 6. clean_rewind_test - stops both servers used in the test, if they're
# still running.
#
# The test script can use the helper functions master_psql and standby_psql
# to run psql against the master and standby servers, respectively.

use strict;
use warnings;

use Carp;
use Config;
use Exporter 'import';
use File::Copy;
use File::Path qw(rmtree);
use IPC::Run qw(run);
use PostgresNode;
use TestLib;
use Test::More;

our @EXPORT = qw(
  $node_master
  $node_standby

  master_psql
  standby_psql
  check_query

  setup_cluster
  start_master
  create_standby
  promote_standby
  run_pg_rewind
  clean_rewind_test
);

# Our nodes.
our $node_master;
our $node_standby;

sub master_psql
{
	my $cmd = shift;
	my $dbname = shift || 'postgres';

	system_or_bail 'psql', '-q', '--no-psqlrc', '-d',
	  $node_master->connstr($dbname), '-c', "$cmd";
	return;
}

sub standby_psql
{
	my $cmd = shift;
	my $dbname = shift || 'postgres';

	system_or_bail 'psql', '-q', '--no-psqlrc', '-d',
	  $node_standby->connstr($dbname), '-c', "$cmd";
	return;
}

# Run a query against the master, and check that the output matches what's
# expected
sub check_query
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($query, $expected_stdout, $test_name) = @_;
	my ($stdout, $stderr);

	# we want just the output, no formatting
	my $result = run [
		'psql', '-q', '-A', '-t', '--no-psqlrc', '-d',
		$node_master->connstr('postgres'),
		'-c', $query
	  ],
	  '>', \$stdout, '2>', \$stderr;

	# We don't use ok() for the exit code and stderr, because we want this
	# check to be just a single test.
	if (!$result)
	{
		fail("$test_name: psql exit code");
	}
	elsif ($stderr ne '')
	{
		diag $stderr;
		fail("$test_name: psql no stderr");
	}
	else
	{
		is($stdout, $expected_stdout, "$test_name: query result matches");
	}
	return;
}

sub setup_cluster
{
	my $extra_name = shift;    # Used to differentiate clusters
	my $extra      = shift;    # Extra params for initdb

	# Initialize master, data checksums are mandatory
	$node_master =
	  get_new_node('master' . ($extra_name ? "_${extra_name}" : ''));

	# Set up pg_hba.conf and pg_ident.conf for the role running
	# pg_rewind.  This role is used for all the tests, and has
	# minimal permissions enough to rewind from an online source.
	$node_master->init(
		allows_streaming => 1,
		extra            => $extra,
		auth_extra       => [ '--create-role', 'rewind_user' ]);

	# Set wal_keep_segments to prevent WAL segment recycling after enforced
	# checkpoints in the tests.
	$node_master->append_conf(
		'postgresql.conf', qq(
wal_keep_segments = 20
));
	return;
}

sub start_master
{
	$node_master->start;

	# Create custom role which is used to run pg_rewind, and adjust its
	# permissions to the minimum necessary.
	$node_master->psql(
		'postgres', "
		CREATE ROLE rewind_user LOGIN;
		GRANT EXECUTE ON function pg_catalog.pg_ls_dir(text, boolean, boolean)
		  TO rewind_user;
		GRANT EXECUTE ON function pg_catalog.pg_stat_file(text, boolean)
		  TO rewind_user;
		GRANT EXECUTE ON function pg_catalog.pg_read_binary_file(text)
		  TO rewind_user;
		GRANT EXECUTE ON function pg_catalog.pg_read_binary_file(text, bigint, bigint, boolean)
		  TO rewind_user;");

	#### Now run the test-specific parts to initialize the master before setting
	# up standby

	return;
}

sub create_standby
{
	my $extra_name = shift;

	$node_standby =
	  get_new_node('standby' . ($extra_name ? "_${extra_name}" : ''));
	$node_master->backup('my_backup');
	$node_standby->init_from_backup($node_master, 'my_backup');
	my $connstr_master = $node_master->connstr();

	$node_standby->append_conf(
		"postgresql.conf", qq(
primary_conninfo='$connstr_master'
));

	$node_standby->set_standby_mode();

	# Start standby
	$node_standby->start;

	# The standby may have WAL to apply before it matches the primary.  That
	# is fine, because no test examines the standby before promotion.

	return;
}

sub promote_standby
{
	#### Now run the test-specific parts to run after standby has been started
	# up standby

	# Wait for the standby to receive and write all WAL.
	$node_master->wait_for_catchup($node_standby, 'write');

	# Now promote standby and insert some new data on master, this will put
	# the master out-of-sync with the standby.
	$node_standby->promote;

	# Force a checkpoint after the promotion. pg_rewind looks at the control
	# file to determine what timeline the server is on, and that isn't updated
	# immediately at promotion, but only at the next checkpoint. When running
	# pg_rewind in remote mode, it's possible that we complete the test steps
	# after promotion so quickly that when pg_rewind runs, the standby has not
	# performed a checkpoint after promotion yet.
	standby_psql("checkpoint");

	return;
}

sub run_pg_rewind
{
	my $test_mode       = shift;
	my $master_pgdata   = $node_master->data_dir;
	my $standby_pgdata  = $node_standby->data_dir;
	my $standby_connstr = $node_standby->connstr('postgres');
	my $tmp_folder      = TestLib::tempdir;

	# Append the rewind-specific role to the connection string.
	$standby_connstr = "$standby_connstr user=rewind_user";

	# Stop the master and be ready to perform the rewind
	$node_master->stop;

	# At this point, the rewind processing is ready to run.
	# We now have a very simple scenario with a few diverged WAL record.
	# The real testing begins really now with a bifurcation of the possible
	# scenarios that pg_rewind supports.

	# Keep a temporary postgresql.conf for master node or it would be
	# overwritten during the rewind.
	copy(
		"$master_pgdata/postgresql.conf",
		"$tmp_folder/master-postgresql.conf.tmp");

	# Now run pg_rewind
	if ($test_mode eq "local")
	{

		# Do rewind using a local pgdata as source
		# Stop the master and be ready to perform the rewind
		$node_standby->stop;
		command_ok(
			[
				'pg_rewind',
				"--debug",
				"--source-pgdata=$standby_pgdata",
				"--target-pgdata=$master_pgdata",
				"--no-sync"
			],
			'pg_rewind local');
	}
	elsif ($test_mode eq "remote")
	{

		# Do rewind using a remote connection as source
		command_ok(
			[
				'pg_rewind',                      "--debug",
				"--source-server",                $standby_connstr,
				"--target-pgdata=$master_pgdata", "--no-sync"
			],
			'pg_rewind remote');
	}
	else
	{

		# Cannot come here normally
		croak("Incorrect test mode specified");
	}

	# Now move back postgresql.conf with old settings
	move(
		"$tmp_folder/master-postgresql.conf.tmp",
		"$master_pgdata/postgresql.conf");

	chmod(
		$node_master->group_access() ? 0640 : 0600,
		"$master_pgdata/postgresql.conf")
	  or BAIL_OUT(
		"unable to set permissions for $master_pgdata/postgresql.conf");

	# Plug-in rewound node to the now-promoted standby node
	my $port_standby = $node_standby->port;
	$node_master->append_conf(
		'postgresql.conf', qq(
primary_conninfo='port=$port_standby'
));

	$node_master->set_standby_mode();

	# Restart the master to check that rewind went correctly
	$node_master->start;

	#### Now run the test-specific parts to check the result

	return;
}

# Clean up after the test. Stop both servers, if they're still running.
sub clean_rewind_test
{
	$node_master->teardown_node  if defined $node_master;
	$node_standby->teardown_node if defined $node_standby;
	return;
}

1;
