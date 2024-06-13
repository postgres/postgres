
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $tempdir_short = PostgreSQL::Test::Utils::tempdir_short;

program_help_ok('pg_ctl');
program_version_ok('pg_ctl');
program_options_handling_ok('pg_ctl');

command_exit_is([ 'pg_ctl', 'start', '-D', "$tempdir/nonexistent" ],
	1, 'pg_ctl start with nonexistent directory');

command_ok([ 'pg_ctl', 'initdb', '-D', "$tempdir/data", '-o', '-N' ],
	'pg_ctl initdb');
command_ok([ $ENV{PG_REGRESS}, '--config-auth', "$tempdir/data" ],
	'configure authentication');
my $node_port = PostgreSQL::Test::Cluster::get_free_port();
open my $conf, '>>', "$tempdir/data/postgresql.conf" or die $!;
print $conf "fsync = off\n";
print $conf "port = $node_port\n";
print $conf PostgreSQL::Test::Utils::slurp_file($ENV{TEMP_CONFIG})
  if defined $ENV{TEMP_CONFIG};

if ($use_unix_sockets)
{
	print $conf "listen_addresses = ''\n";
	$tempdir_short =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;
	print $conf "unix_socket_directories = '$tempdir_short'\n";
}
else
{
	print $conf "listen_addresses = '127.0.0.1'\n";
}
close $conf;
my $ctlcmd = [
	'pg_ctl', 'start', '-D', "$tempdir/data", '-l',
	"$PostgreSQL::Test::Utils::log_path/001_start_stop_server.log"
];
command_like($ctlcmd, qr/done.*server started/s, 'pg_ctl start');

# sleep here is because Windows builds can't check postmaster.pid exactly,
# so they may mistake a pre-existing postmaster.pid for one created by the
# postmaster they start.  Waiting more than the 2 seconds slop time allowed
# by wait_for_postmaster() prevents that mistake.
sleep 3 if ($windows_os);
command_fails([ 'pg_ctl', 'start', '-D', "$tempdir/data" ],
	'second pg_ctl start fails');
command_ok([ 'pg_ctl', 'stop', '-D', "$tempdir/data" ], 'pg_ctl stop');
command_fails([ 'pg_ctl', 'stop', '-D', "$tempdir/data" ],
	'second pg_ctl stop fails');

# Log file for default permission test.  The permissions won't be checked on
# Windows but we still want to do the restart test.
my $logFileName = "$tempdir/data/perm-test-600.log";

command_ok([ 'pg_ctl', 'restart', '-D', "$tempdir/data", '-l', $logFileName ],
	'pg_ctl restart with server not running');

# Permissions on log file should be default
SKIP:
{
	skip "unix-style permissions not supported on Windows", 2
	  if ($windows_os);

	ok(-f $logFileName);
	ok(check_mode_recursive("$tempdir/data", 0700, 0600));
}

# Log file for group access test
$logFileName = "$tempdir/data/perm-test-640.log";

SKIP:
{
	skip "group access not supported on Windows", 3
	  if ($windows_os || $Config::Config{osname} eq 'cygwin');

	system_or_bail 'pg_ctl', 'stop', '-D', "$tempdir/data";

	# Change the data dir mode so log file will be created with group read
	# privileges on the next start
	chmod_recursive("$tempdir/data", 0750, 0640);

	command_ok(
		[ 'pg_ctl', 'start', '-D', "$tempdir/data", '-l', $logFileName ],
		'start server to check group permissions');

	ok(-f $logFileName);
	ok(check_mode_recursive("$tempdir/data", 0750, 0640));
}

command_ok([ 'pg_ctl', 'restart', '-D', "$tempdir/data" ],
	'pg_ctl restart with server running');

system_or_bail 'pg_ctl', 'stop', '-D', "$tempdir/data";

done_testing();
