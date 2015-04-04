use strict;
use warnings;
use TestLib;
use Test::More tests => 13;

program_help_ok('createdb');
program_version_ok('createdb');
program_options_handling_ok('createdb');

my $tempdir = tempdir;
start_test_server $tempdir;

issues_sql_like(
	[ 'createdb', 'foobar1' ],
	qr/statement: CREATE DATABASE foobar1/,
	'SQL CREATE DATABASE run');
issues_sql_like(
	[ 'createdb', '-l', 'C', '-E', 'LATIN1', '-T', 'template0', 'foobar2' ],
	qr/statement: CREATE DATABASE foobar2 ENCODING 'LATIN1'/,
	'create database with encoding');

command_fails([ 'createdb', 'foobar1' ], 'fails if database already exists');
