# Copyright (c) 2025-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize a cluster with the extension installed.  The tests will
# call the function that comes with the extension to load it.
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;
$node->safe_psql("postgres", "CREATE EXTENSION test_shmem");
$node->stop;

###
# Test allocating memory after startup, i.e. when the library is not
# in shared_preload_libraries
###
$node->start;

# Check that the attach counter is incremented on a new connection
my $attach_count1 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
my $attach_count2 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
cmp_ok($attach_count2, '>', $attach_count1,
	"attach callback is called in each backend");

$node->stop;

###
# Test allocating memory after startup in single-user mode
###
SKIP:
{
	# Skip the test on Windows, as single-user mode would fail on permission
	# failure with privileged accounts.
	skip 'single-user test is not supported by this platform', 1
	  if $windows_os;
	my $query = "SELECT get_test_shmem_attach_count();\n";
	my $result = run_log(
		[
			'postgres', '--single', '-F',
			'-c' => 'exit_on_error=true',
			'-D' => $node->data_dir,
			'postgres'
		],
		'<' => \$query);

	ok($result, "shmem area is initialized in single-user mode");
}

###
# Test that loading via shared_preload_libraries also works
###
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_shmem'");
$node->start;

# When loaded via shared_preload_libraries, the attach callback is
# called or not, depending on whether this is an EXEC_BACKEND build.
my $exec_backend =
  $node->safe_psql("postgres", "SHOW debug_exec_backend;") eq 'on';
$attach_count1 =
  $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
$attach_count2 =
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

# clean up
$node->stop;
$node->adjust_conf('postgresql.conf', "shared_preload_libraries", undef);

###
# Test a failure in initializing the shared memory area
###
SKIP:
{
	skip "injection points not supported by this build",
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

done_testing();
