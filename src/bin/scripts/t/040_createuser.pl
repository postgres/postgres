use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 17;

program_help_ok('createuser');
program_version_ok('createuser');
program_options_handling_ok('createuser');

my $node = get_new_node('main');
$node->init;
$node->start;

$node->issues_sql_like(
	[ 'createuser', 'user1' ],
qr/statement: CREATE ROLE user1 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN;/,
	'SQL CREATE USER run');
$node->issues_sql_like(
	[ 'createuser', '-L', 'role1' ],
qr/statement: CREATE ROLE role1 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT NOLOGIN;/,
	'create a non-login role');
$node->issues_sql_like(
	[ 'createuser', '-r', 'user2' ],
qr/statement: CREATE ROLE user2 NOSUPERUSER NOCREATEDB CREATEROLE INHERIT LOGIN;/,
	'create a CREATEROLE user');
$node->issues_sql_like(
	[ 'createuser', '-s', 'user3' ],
qr/statement: CREATE ROLE user3 SUPERUSER CREATEDB CREATEROLE INHERIT LOGIN;/,
	'create a superuser');

$node->command_fails([ 'createuser', 'user1' ],
	'fails if role already exists');
