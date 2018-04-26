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
	[ 'createuser', 'regress_user1' ],
	qr/statement: CREATE ROLE regress_user1 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN;/,
	'SQL CREATE USER run');
$node->issues_sql_like(
	[ 'createuser', '-L', 'regress_role1' ],
	qr/statement: CREATE ROLE regress_role1 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT NOLOGIN;/,
	'create a non-login role');
$node->issues_sql_like(
	[ 'createuser', '-r', 'regress_user2' ],
	qr/statement: CREATE ROLE regress_user2 NOSUPERUSER NOCREATEDB CREATEROLE INHERIT LOGIN;/,
	'create a CREATEROLE user');
$node->issues_sql_like(
	[ 'createuser', '-s', 'regress_user3' ],
	qr/statement: CREATE ROLE regress_user3 SUPERUSER CREATEDB CREATEROLE INHERIT LOGIN;/,
	'create a superuser');

$node->command_fails([ 'createuser', 'regress_user1' ],
	'fails if role already exists');
