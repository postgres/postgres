use strict;
use warnings;
use TestLib;
use Test::More tests => 3;

my $tempdir = TestLib::tempdir;
my $tempdir_short = TestLib::tempdir_short;

command_exit_is([ 'pg_ctl', 'status', '-D', "$tempdir/nonexistent" ],
	4, 'pg_ctl status with nonexistent directory');

system_or_bail "initdb -D '$tempdir'/data -A trust >/dev/null";
open CONF, ">>$tempdir/data/postgresql.conf";
print CONF "listen_addresses = ''\n";
print CONF "unix_socket_directories = '$tempdir_short'\n";
close CONF;

command_exit_is([ 'pg_ctl', 'status', '-D', "$tempdir/data" ],
	3, 'pg_ctl status with server not running');

system_or_bail 'pg_ctl', '-s', '-l', "$tempdir/logfile", '-D',
  "$tempdir/data", '-w', 'start';
command_exit_is([ 'pg_ctl', 'status', '-D', "$tempdir/data" ],
	0, 'pg_ctl status with server running');

system_or_bail 'pg_ctl', '-s', 'stop', '-D', "$tempdir/data", '-m', 'fast';
