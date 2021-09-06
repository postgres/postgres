
# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 2;
use Time::HiRes qw(usleep);

my $tempdir = TestLib::tempdir;

my $node = PostgresNode->new('main');
$node->init;
$node->start;

# Test query canceling by sending SIGINT to a running psql
#
# There is, as of this writing, no documented way to get the PID of
# the process from IPC::Run.  As a workaround, we have psql print its
# own PID (which is the parent of the shell launched by psql) to a
# file.
SKIP: {
	skip "cancel test requires a Unix shell", 2 if $windows_os;

	local %ENV = $node->_get_env();

	my ($stdin, $stdout, $stderr);

	# Test whether shell supports $PPID.  It's part of POSIX, but some
	# pre-/non-POSIX shells don't support it (e.g., NetBSD).
	$stdin = "\\! echo \$PPID";
	IPC::Run::run(['psql', '-X', '-v', 'ON_ERROR_STOP=1'], '<', \$stdin, '>', \$stdout, '2>', \$stderr);
	$stdout =~ /^\d+$/ or skip "shell apparently does not support \$PPID", 2;

	# Now start the real test
	my $h = IPC::Run::start(['psql', '-X', '-v', 'ON_ERROR_STOP=1'], \$stdin, \$stdout, \$stderr);

	# Get the PID
	$stdout = '';
	$stderr = '';
	$stdin = "\\! echo \$PPID >$tempdir/psql.pid\n";
	pump $h while length $stdin;
	my $count;
	my $psql_pid;
	until (-s "$tempdir/psql.pid" and ($psql_pid = TestLib::slurp_file("$tempdir/psql.pid")) =~ /^\d+\n/s)
	{
		($count++ < 180 * 100) or die "pid file did not appear";
		usleep(10_000)
	}

	# Send sleep command and wait until the server has registered it
	$stdin = "select pg_sleep(180);\n";
	pump $h while length $stdin;
	$node->poll_query_until('postgres', q{SELECT (SELECT count(*) FROM pg_stat_activity WHERE query ~ '^select pg_sleep') > 0;})
	  or die "timed out";

	# Send cancel request
	kill 'INT', $psql_pid;

	my $result = finish $h;

	ok(!$result, 'query failed as expected');
	like($stderr, qr/canceling statement due to user request/, 'query was canceled');
}
