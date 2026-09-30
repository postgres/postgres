# Copyright (c) 2025-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize a cluster with the extension installed.  Some tests below will
# load the library via shared_preload_libraries, while others will load it
# after startup by calling the extension's functions.
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;
$node->safe_psql("postgres", "CREATE EXTENSION test_shmem");
$node->stop;


###
# Test that loading via shared_preload_libraries works
###
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_shmem'");
$node->start;

# When loaded via shared_preload_libraries, the attach callback is
# called or not, depending on whether this is an EXEC_BACKEND build.
my $exec_backend =
  $node->safe_psql("postgres", "SHOW debug_exec_backend;") eq 'on';
my $attach_count1 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
my $attach_count2 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");

if ($exec_backend)
{
	cmp_ok($attach_count2, '>', $attach_count1,
		"attach callback is called in each backend when loaded via shared_preload_libraries"
	);
}
else
{
	ok( $attach_count1 == 0 && $attach_count2 == 0,
		"attach callback is not called when loaded via shared_preload_libraries"
	);
}

$node->stop;
$node->adjust_conf('postgresql.conf', 'shared_preload_libraries', undef);


###
# Test allocating memory after startup, i.e. when the library is not
# in shared_preload_libraries
###
$node->start;

# This first call to the function after startup loads the library
# and initializes the shmem area.
$attach_count1 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");

# Check that the attach counter is incremented on a new connection
$attach_count2 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
cmp_ok($attach_count2, '>', $attach_count1,
	"attach callback is called in each backend");

# Allocate another shmem area, after the library has been loaded
my $res = $node->safe_psql("postgres",
	"SELECT test_shmem_register('test_shmem after startup', 20, 1);");
is($res, 0, 'allocate after startup');

# Test attaching to it again
$res = $node->safe_psql("postgres",
	"SELECT test_shmem_register('test_shmem after startup', 20, 2);");
is($res, 1, 'attach after startup');

# If the size doesn't match when attaching, you get an error
my (undef, undef, $stderr) =
  $node->psql("postgres",
	"SELECT test_shmem_register('test_shmem after startup', 25, 3);");
like(
	$stderr,
	qr/ERROR:  shared memory struct "test_shmem after startup" was created with different size: existing 20, requested 25/,
	"attaching with different size fails");

# Test attaching with SHMEM_ATTACH_UNKNOWN_SIZE
$res =
  $node->safe_psql("postgres",
	"SELECT test_shmem_register('test_shmem after startup', -1, 4);");
is($res, 2, 'attach with SHMEM_ATTACH_UNKNOWN_SIZE');

# Cannot use SHMEM_ATTACH_UNKNOWN_SIZE with a segment that doesn't exist
(undef, undef, $stderr) =
  $node->psql("postgres",
	"SELECT test_shmem_register('test_shmem missing area', -1, 5);");
like(
	$stderr,
	qr/ERROR:  cannot attach to shared memory struct "test_shmem missing area" because it does not exist/,
	"unknown-size request for a nonexistent area fails");

$node->stop;


###
# Test a failure in initializing the shared memory area
###
SKIP:
{
	skip "injection points not supported by this build", 2
	  if $ENV{enable_injection_points} ne 'yes';
	$node->start;
	$node->safe_psql("postgres", "CREATE EXTENSION injection_points;");
	$node->safe_psql("postgres",
		"SELECT injection_points_attach('test-shmem-init', 'error');");

	# Try to load the extension library. It will hit the injected
	# error in the init callback.
	my (undef, undef, $stderr) =
	  $node->psql("postgres", "SELECT get_test_shmem_attach_count();");
	like(
		$stderr,
		qr/error triggered for injection point test-shmem-init/,
		"failure in initialization is reported");
	$node->safe_psql("postgres",
		"SELECT injection_points_detach('test-shmem-init');");

	# The error leaves the shared memory area in a broken state.
	# Attempting to initialize or attach it again will fail, until the
	# server is restarted.
	(undef, undef, $stderr) =
	  $node->psql("postgres", "SELECT get_test_shmem_attach_count();");
	like(
		$stderr,
		qr/cannot attach to shared memory/,
		"post-init extension creation fails");

	$node->stop;
}


###
# Test "out of shared memory" in an after-startup request
###

# Huge pages round up the main shared memory segment, which can leave more
# unused space than the request below.  Disable them to make the test reliable.
$node->append_conf('postgresql.conf', 'huge_pages = off');
$node->start;
my $session = $node->background_psql('postgres', on_error_stop => 0);

# make the request larger than the memory reserved for after-startup
# requests.
$session->query(q[SET test_shmem.area_size = '128kB';]);

$session->query("SELECT get_test_shmem_attach_count();");
like(
	$session->{stderr},
	qr/not enough shared memory/,
	"an after-startup request larger than the reserve fails");

# The server and the backend keep running.  Since only one area was
# requested, it gets cleaned up on allocation failure.  Verify that a
# request for a smaller area succeeds in the same session.
$session->{stderr} = '';
$session->query("SET test_shmem.area_size = default;");
$session->query_safe("SELECT get_test_shmem_attach_count();");
$session->quit;
$node->stop;


###
# Test shmem allocations in single-user mode
###
SKIP:
{
	# Skip the test on Windows, as single-user mode would fail on permission
	# failure with privileged accounts.
	skip 'single-user test is not supported by this platform', 4
	  if $windows_os;

	my @command = (
		'postgres', '--single', '-F',
		'-c' => 'exit_on_error=true',
		'-D' => $node->data_dir);

	# The call to get_test_shmem_attach_count() loads the library and
	# allocates the shmem area
	my $queries = "SELECT get_test_shmem_attach_count();\n";
	my $result = run_log([ @command, 'postgres' ], '<' => \$queries);
	ok($result, "shmem area is initialized in single-user mode");

	# Test allocate and attach requests in a single-user session
	$queries = qq{
-- allocate
SELECT test_shmem_register('test_shmem after startup', 25, 1);
-- attach
SELECT test_shmem_register('test_shmem after startup', 25, 2);
-- attach with SHMEM_ATTACH_UNKNOWN_SIZE
SELECT test_shmem_register('test_shmem after startup', -1, 3);
};
	$result = run_log([ @command, 'postgres' ], '<' => \$queries);
	ok($result, "allocate and attach in single-user mode");

	# Cannot use SHMEM_ATTACH_UNKNOWN_SIZE with a segment that doesn't exist.
	# We tested this in the after-startup tests already, but test it at startup,
	# in single-user mode, too.
	my $startup_stderr;
	$result = run_log(
		[
			@command,
			'-c' => 'shared_preload_libraries=test_shmem',
			'-c' => 'test_shmem.area_size=-1',
			'postgres'
		],
		'<' => \$queries,
		'2>' => \$startup_stderr);
	ok(!$result,
		"unknown-size request during single-user startup is rejected");
	like(
		$startup_stderr,
		qr/SHMEM_ATTACH_UNKNOWN_SIZE cannot be used during startup/,
		"error message on rejected unknown-size request");
}

done_testing();
