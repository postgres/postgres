use strict;
use warnings;
use TestLib;
use Test::More tests => 16;

my $tempdir = TestLib::tempdir;
my $tempdir_short = TestLib::tempdir_short;

program_help_ok('pg_ctl');
program_version_ok('pg_ctl');
program_options_handling_ok('pg_ctl');

command_ok([ 'pg_ctl', 'initdb', '-D', "$tempdir/data" ], 'pg_ctl initdb');
command_ok(
	[   "$ENV{top_builddir}/src/test/regress/pg_regress", '--config-auth',
		"$tempdir/data" ],
	'configure authentication');
open CONF, ">>$tempdir/data/postgresql.conf";
print CONF "listen_addresses = ''\n";
print CONF "unix_socket_directories = '$tempdir_short'\n";
close CONF;
command_ok([ 'pg_ctl', 'start', '-D', "$tempdir/data", '-w' ],
	'pg_ctl start -w');
command_ok([ 'pg_ctl', 'start', '-D', "$tempdir/data", '-w' ],
	'second pg_ctl start succeeds');
command_ok([ 'pg_ctl', 'stop', '-D', "$tempdir/data", '-w', '-m', 'fast' ],
	'pg_ctl stop -w');
command_fails([ 'pg_ctl', 'stop', '-D', "$tempdir/data", '-w', '-m', 'fast' ],
	'second pg_ctl stop fails');

command_ok([ 'pg_ctl', 'restart', '-D', "$tempdir/data", '-w', '-m', 'fast' ],
	'pg_ctl restart with server not running');
command_ok([ 'pg_ctl', 'restart', '-D', "$tempdir/data", '-w', '-m', 'fast' ],
	'pg_ctl restart with server running');

system_or_bail 'pg_ctl', '-s', 'stop', '-D', "$tempdir/data", '-m', 'fast';
