use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;
use Test::More tests => 19;

my $tempdir       = TestLib::tempdir;
my $tempdir_short = TestLib::tempdir_short;

program_help_ok('pg_ctl');
program_version_ok('pg_ctl');
program_options_handling_ok('pg_ctl');

command_exit_is([ 'pg_ctl', 'start', '-D', "$tempdir/nonexistent" ],
	1, 'pg_ctl start with nonexistent directory');

command_ok([ 'pg_ctl', 'initdb', '-D', "$tempdir/data", '-o', '-N' ],
	'pg_ctl initdb');
command_ok([ $ENV{PG_REGRESS}, '--config-auth', "$tempdir/data" ],
	'configure authentication');
open my $conf, '>>', "$tempdir/data/postgresql.conf";
print $conf "fsync = off\n";
if (!$windows_os)
{
	print $conf "listen_addresses = ''\n";
	print $conf "unix_socket_directories = '$tempdir_short'\n";
}
else
{
	print $conf "listen_addresses = '127.0.0.1'\n";
}
close $conf;
my $ctlcmd = [
	'pg_ctl', 'start', '-D', "$tempdir/data", '-l',
	"$TestLib::log_path/001_start_stop_server.log" ];
if ($Config{osname} ne 'msys')
{
	command_like($ctlcmd, qr/done.*server started/s, 'pg_ctl start');
}
else
{

	# use the version of command_like that doesn't hang on Msys here
	command_like_safe($ctlcmd, qr/done.*server started/s, 'pg_ctl start');
}

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

command_ok(
	[ 'pg_ctl', 'restart', '-D', "$tempdir/data" ],
	'pg_ctl restart with server not running');
command_ok([ 'pg_ctl', 'restart', '-D', "$tempdir/data" ],
	'pg_ctl restart with server running');

system_or_bail 'pg_ctl', 'stop', '-D', "$tempdir/data";
