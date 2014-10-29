use strict;
use warnings;
use TestLib;
use Test::More tests => 14;

program_help_ok('createlang');
program_version_ok('createlang');
program_options_handling_ok('createlang');

my $tempdir = tempdir;
start_test_server $tempdir;

command_fails(
	[ 'createlang', 'plpgsql', 'postgres' ],
	'fails if language already exists');

psql 'postgres', 'DROP EXTENSION plpgsql';
issues_sql_like(
	[ 'createlang', 'plpgsql', 'postgres' ],
	qr/statement: CREATE EXTENSION "plpgsql"/,
	'SQL CREATE EXTENSION run');

command_like([ 'createlang', '--list', 'postgres' ],
	qr/plpgsql/, 'list output');
