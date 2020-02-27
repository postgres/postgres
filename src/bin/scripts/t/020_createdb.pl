use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 19;

program_help_ok('createdb');
program_version_ok('createdb');
program_options_handling_ok('createdb');

my $node = get_new_node('main');
$node->init;
$node->start;

$node->issues_sql_like(
	[ 'createdb', 'foobar1' ],
	qr/statement: CREATE DATABASE foobar1/,
	'SQL CREATE DATABASE run');
$node->issues_sql_like(
	[ 'createdb', '-l', 'C', '-E', 'LATIN1', '-T', 'template0', 'foobar2' ],
	qr/statement: CREATE DATABASE foobar2 ENCODING 'LATIN1'/,
	'create database with encoding');

$node->command_fails([ 'createdb', 'foobar1' ],
	'fails if database already exists');

# Check quote handling with incorrect option values.
$node->command_checks_all(
	[ 'createdb', '--encoding', "foo'; SELECT '1", 'foobar2' ],
	1,
	[qr/^$/],
	[qr/^createdb: error: "foo'; SELECT '1" is not a valid encoding name/s],
	'createdb with incorrect --lc-collate');
$node->command_checks_all(
	[ 'createdb', '--lc-collate', "foo'; SELECT '1", 'foobar2' ],
	1,
	[qr/^$/],
	[
		qr/^createdb: error: database creation failed: ERROR:  invalid locale name/s
	],
	'createdb with incorrect --lc-collate');
