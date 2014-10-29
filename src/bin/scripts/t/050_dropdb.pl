use strict;
use warnings;
use TestLib;
use Test::More tests => 11;

program_help_ok('dropdb');
program_version_ok('dropdb');
program_options_handling_ok('dropdb');

my $tempdir = tempdir;
start_test_server $tempdir;

psql 'postgres', 'CREATE DATABASE foobar1';
issues_sql_like(
	[ 'dropdb', 'foobar1' ],
	qr/statement: DROP DATABASE foobar1/,
	'SQL DROP DATABASE run');

command_fails([ 'dropdb', 'nonexistent' ], 'fails with nonexistent database');
