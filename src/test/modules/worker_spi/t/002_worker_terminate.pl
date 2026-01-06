# Copyright (c) 2026, PostgreSQL Global Development Group

# Test background workers can be terminated by db commands

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# This test depends on injection points to detect whether background workers
# remain.
if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Ensure the worker_spi dynamic worker is launched on the specified database.
# Returns the PID of the worker launched.
sub launch_bgworker
{
	my ($node, $database, $testcase, $interruptible) = @_;
	my $offset = -s $node->logfile;

	# Launch a background worker on the given database.
	my $pid = $node->safe_psql(
		$database, qq(
        SELECT worker_spi_launch($testcase, '$database'::regdatabase, 0, '{}', $interruptible);
    ));

	# Check that the bgworker is initialized.
	$node->wait_for_log(
		qr/LOG: .*worker_spi dynamic worker $testcase initialized with .*\..*/,
		$offset);
	my $result = $node->safe_psql($database,
		"SELECT count(*) > 0 FROM pg_stat_activity WHERE pid = $pid;");
	is($result, 't', "dynamic bgworker $testcase launched");

	return $pid;
}

# Run query and verify that the bgworker with the specified PID has been
# terminated.
sub run_bgworker_interruptible_test
{
	my ($node, $command, $testname, $pid) = @_;
	my $offset = -s $node->logfile;

	$node->safe_psql('postgres', $command);

	$node->wait_for_log(
		qr/terminating background worker \"worker_spi dynamic\" due to administrator command/,
		$offset);

	my $result = $node->safe_psql('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid;");
	is($result, 't', "dynamic bgworker stopped for $testname");
}

my $node = PostgreSQL::Test::Cluster->new('mynode');
$node->init;
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION worker_spi;');

# Launch a background worker without BGWORKER_INTERRUPTIBLE.
my $pid = launch_bgworker($node, 'postgres', 0, 'false');

# Ensure CREATE DATABASE WITH TEMPLATE fails because a non-interruptible
# bgworker exists.

# The injection point 'procarray-reduce-count' reduces the number of backend
# retries, allowing for shorter test runs. See CountOtherDBBackends().
$node->safe_psql('postgres', "CREATE EXTENSION injection_points;");
$node->safe_psql('postgres',
	"SELECT injection_points_attach('procarray-reduce-count', 'error');");

my $stderr;

$node->psql(
	'postgres',
	"CREATE DATABASE testdb WITH TEMPLATE postgres",
	stderr => \$stderr);
ok( $stderr =~
	  "source database \"postgres\" is being accessed by other users",
	"background worker blocked the database creation");

# Confirm that the non-interruptible bgworker is still running.
my $result = $node->safe_psql(
	"postgres", qq(
        SELECT count(1) FROM pg_stat_activity
        WHERE backend_type = 'worker_spi dynamic';));

is($result, '1',
	"background worker is still running after CREATE DATABASE WITH TEMPLATE");

# Terminate the non-interruptible worker for the next tests.
$node->safe_psql(
	"postgres", qq(
        SELECT pg_terminate_backend(pid)
        FROM pg_stat_activity WHERE backend_type = 'worker_spi dynamic';));

# The injection point is not used anymore, release it.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('procarray-reduce-count');");

# Check that BGWORKER_INTERRUPTIBLE allows background workers to be
# terminated with database-related commands.

# Test case 1: CREATE DATABASE WITH TEMPLATE
$pid = launch_bgworker($node, 'postgres', 1, 'true');
run_bgworker_interruptible_test(
	$node,
	"CREATE DATABASE testdb WITH TEMPLATE postgres",
	"CREATE DATABASE WITH TEMPLATE", $pid);

# Test case 2: ALTER DATABASE RENAME
$pid = launch_bgworker($node, 'testdb', 2, 'true');
run_bgworker_interruptible_test(
	$node,
	"ALTER DATABASE testdb RENAME TO renameddb",
	"ALTER DATABASE RENAME", $pid);

# Preparation for the next test, create a tablespace.
my $tablespace = PostgreSQL::Test::Utils::tempdir;
$node->safe_psql('postgres',
	"CREATE TABLESPACE test_tablespace LOCATION '$tablespace'");

# Test case 3: ALTER DATABASE SET TABLESPACE
$pid = launch_bgworker($node, 'renameddb', 3, 'true');
run_bgworker_interruptible_test(
	$node,
	"ALTER DATABASE renameddb SET TABLESPACE test_tablespace",
	"ALTER DATABASE SET TABLESPACE", $pid);

# Test case 4: DROP DATABASE
$pid = launch_bgworker($node, 'renameddb', 4, 'true');
run_bgworker_interruptible_test(
	$node,
	"DROP DATABASE renameddb",
	"DROP DATABASE", $pid);

done_testing();
