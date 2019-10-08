#
# Test checking options of pg_rewind.
#
use strict;
use warnings;
use TestLib;
use Test::More tests => 12;

program_help_ok('pg_rewind');
program_version_ok('pg_rewind');
program_options_handling_ok('pg_rewind');

my $primary_pgdata = TestLib::tempdir;
my $standby_pgdata = TestLib::tempdir;
command_fails(
	[
		'pg_rewind',       '--debug',
		'--target-pgdata', $primary_pgdata,
		'--source-pgdata', $standby_pgdata,
		'extra_arg1'
	],
	'too many arguments');
command_fails([ 'pg_rewind', '--target-pgdata', $primary_pgdata ],
	'no source specified');
command_fails(
	[
		'pg_rewind',       '--debug',
		'--target-pgdata', $primary_pgdata,
		'--source-pgdata', $standby_pgdata,
		'--source-server', 'incorrect_source'
	],
	'both remote and local sources specified');
command_fails(
	[
		'pg_rewind',       '--debug',
		'--target-pgdata', $primary_pgdata,
		'--source-pgdata', $standby_pgdata,
		'--write-recovery-conf'
	],
	'no local source with --write-recovery-conf');
