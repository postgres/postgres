use strict;
use warnings;
use TestLib;
use Test::More tests => 19;

my $tempdir = TestLib::tempdir;

program_help_ok('initdb');
program_version_ok('initdb');
program_options_handling_ok('initdb');

command_ok([ 'initdb', "$tempdir/data" ], 'basic initdb');
command_fails([ 'initdb', "$tempdir/data" ], 'existing data directory');
command_ok([ 'initdb', '-N', "$tempdir/data2" ], 'nosync');
command_ok([ 'initdb', '-S', "$tempdir/data2" ], 'sync only');
command_fails([ 'initdb', '-S', "$tempdir/data3" ],
	'sync missing data directory');
mkdir "$tempdir/data4" or BAIL_OUT($!);
command_ok([ 'initdb', "$tempdir/data4" ], 'existing empty data directory');

system_or_bail "rm -rf '$tempdir'/*";

command_ok([ 'initdb', '-X', "$tempdir/pgxlog", "$tempdir/data" ],
	'separate xlog directory');

system_or_bail "rm -rf '$tempdir'/*";
command_fails(
	[ 'initdb', '-X', 'pgxlog', "$tempdir/data" ],
	'relative xlog directory not allowed');

system_or_bail "rm -rf '$tempdir'/*";
mkdir "$tempdir/pgxlog";
command_ok([ 'initdb', '-X', "$tempdir/pgxlog", "$tempdir/data" ],
	'existing empty xlog directory');

system_or_bail "rm -rf '$tempdir'/*";
mkdir "$tempdir/pgxlog";
mkdir "$tempdir/pgxlog/lost+found";
command_fails([ 'initdb', '-X', "$tempdir/pgxlog", "$tempdir/data" ],
	'existing nonempty xlog directory');

system_or_bail "rm -rf '$tempdir'/*";
command_ok([ 'initdb', '-T', 'german', "$tempdir/data" ],
	'select default dictionary');
