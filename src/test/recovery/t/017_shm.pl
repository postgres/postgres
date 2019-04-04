#
# Tests of pg_shmem.h functions
#
use strict;
use warnings;
use IPC::Run 'run';
use PostgresNode;
use Test::More;
use TestLib;

plan tests => 6;

my $tempdir = TestLib::tempdir;
my $port;

# When using Unix sockets, we can dictate the port number.  In the absence of
# collisions from other shmget() activity, gnat starts with key 0x7d001
# (512001), and flea starts with key 0x7d002 (512002).
$port = 512 unless $PostgresNode::use_tcp;

# Log "ipcs" diffs on a best-effort basis, swallowing any error.
my $ipcs_before = "$tempdir/ipcs_before";
eval { run_log [ 'ipcs', '-am' ], '>', $ipcs_before; };

sub log_ipcs
{
	eval { run_log [ 'ipcs', '-am' ], '|', [ 'diff', $ipcs_before, '-' ] };
	return;
}

# Node setup.
sub init_start
{
	my $name = shift;
	my $ret = PostgresNode->get_new_node($name, port => $port, own_host => 1);
	defined($port) or $port = $ret->port;    # same port for all nodes
	$ret->init;
	$ret->start;
	log_ipcs();
	return $ret;
}
my $gnat = init_start 'gnat';
my $flea = init_start 'flea';

# Upon postmaster death, postmaster children exit automatically.
$gnat->kill9;
log_ipcs();
$flea->restart;       # flea ignores the shm key gnat abandoned.
log_ipcs();
poll_start($gnat);    # gnat recycles its former shm key.
log_ipcs();

# After clean shutdown, the nodes swap shm keys.
$gnat->stop;
$flea->restart;
log_ipcs();
$gnat->start;
log_ipcs();

# Scenarios involving no postmaster.pid, dead postmaster, and a live backend.
# Use a regress.c function to emulate the responsiveness of a backend working
# through a CPU-intensive task.
$gnat->safe_psql('postgres', <<EOSQL);
CREATE FUNCTION wait_pid(int)
   RETURNS void
   AS '$ENV{REGRESS_SHLIB}'
   LANGUAGE C STRICT;
EOSQL
my $slow_query = 'SELECT wait_pid(pg_backend_pid())';
my ($stdout, $stderr);
my $slow_client = IPC::Run::start(
	[
		'psql', '-X', '-qAt', '-d', $gnat->connstr('postgres'),
		'-c', $slow_query
	],
	'<',
	\undef,
	'>',
	\$stdout,
	'2>',
	\$stderr,
	IPC::Run::timeout(900));    # five times the poll_query_until timeout
ok( $gnat->poll_query_until(
		'postgres',
		"SELECT 1 FROM pg_stat_activity WHERE query = '$slow_query'", '1'),
	'slow query started');
my $slow_pid = $gnat->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE query = '$slow_query'");
$gnat->kill9;
unlink($gnat->data_dir . '/postmaster.pid');
$gnat->rotate_logfile;    # on Windows, can't open old log for writing
log_ipcs();
# Reject ordinary startup.
ok(!$gnat->start(fail_ok => 1), 'live query blocks restart');
like(
	slurp_file($gnat->logfile),
	qr/pre-existing shared memory block/,
	'detected live backend via shared memory');
# Reject single-user startup.
my $single_stderr;
ok( !run_log(
		[ 'postgres', '--single', '-D', $gnat->data_dir, 'template1' ],
		'<', \('SELECT 1 + 1'), '2>', \$single_stderr),
	'live query blocks --single');
print STDERR $single_stderr;
like(
	$single_stderr,
	qr/pre-existing shared memory block/,
	'single-user mode detected live backend via shared memory');
log_ipcs();
# Fail to reject startup if shm key N has become available and we crash while
# using key N+1.  This is unwanted, but expected.  Windows is immune, because
# its GetSharedMemName() use DataDir strings, not numeric keys.
$flea->stop;    # release first key
is( $gnat->start(fail_ok => 1),
	$TestLib::windows_os ? 0 : 1,
	'key turnover fools only sysv_shmem.c');
$gnat->stop;     # release first key (no-op on $TestLib::windows_os)
$flea->start;    # grab first key
# cleanup
TestLib::system_log('pg_ctl', 'kill', 'QUIT', $slow_pid);
$slow_client->finish;    # client has detected backend termination
log_ipcs();
poll_start($gnat);       # recycle second key

$gnat->stop;
$flea->stop;
log_ipcs();


# When postmaster children are slow to exit after postmaster death, we may
# need retries to start a new postmaster.
sub poll_start
{
	my ($node) = @_;

	my $max_attempts = 180 * 10;
	my $attempts     = 0;

	while ($attempts < $max_attempts)
	{
		$node->start(fail_ok => 1) && return 1;

		# Wait 0.1 second before retrying.
		usleep(100_000);

		$attempts++;
	}

	# No success within 180 seconds.  Try one last time without fail_ok, which
	# will BAIL_OUT unless it succeeds.
	$node->start && return 1;
	return 0;
}
