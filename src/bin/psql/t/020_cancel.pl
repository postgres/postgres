
# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 2;

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
	# pre-/non-POSIX shells don't support it (e.g., NetBSD, Solaris).
	$stdin = "\\! echo \$PPID";
	IPC::Run::run(['psql', '-X', '-v', 'ON_ERROR_STOP=1'], '<', \$stdin, '>', \$stdout, '2>', \$stderr);
	$stdout =~ /^\d+$/ or skip "shell apparently does not support \$PPID", 2;

	local $SIG{ALRM} = sub {
		my $psql_pid = TestLib::slurp_file("$tempdir/psql.pid");
		kill 'INT', $psql_pid;
	};
	alarm 1;

	$stdin = "\\! echo \$PPID >$tempdir/psql.pid\nselect pg_sleep(3);";
	my $result = IPC::Run::run(['psql', '-X', '-v', 'ON_ERROR_STOP=1'], '<', \$stdin, '>', \$stdout, '2>', \$stderr);

	ok(!$result, 'query failed as expected');
	like($stderr, qr/canceling statement due to user request/, 'query was canceled');
}
