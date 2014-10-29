use strict;
use warnings;
use TestLib;
use Test::More tests => 11;

program_help_ok('dropuser');
program_version_ok('dropuser');
program_options_handling_ok('dropuser');

my $tempdir = tempdir;
start_test_server $tempdir;

psql 'postgres', 'CREATE ROLE foobar1';
issues_sql_like(
	[ 'dropuser', 'foobar1' ],
	qr/statement: DROP ROLE foobar1/,
	'SQL DROP ROLE run');

command_fails([ 'dropuser', 'nonexistent' ], 'fails with nonexistent user');
