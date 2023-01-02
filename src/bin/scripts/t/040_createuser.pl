
# Copyright (c) 2021-2023, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('createuser');
program_version_ok('createuser');
program_options_handling_ok('createuser');

my $node = PostgreSQL::Test::Cluster->new('main');
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
	[ 'createuser', '-r', 'regress user2' ],
	qr/statement: CREATE ROLE "regress user2" NOSUPERUSER NOCREATEDB CREATEROLE INHERIT LOGIN;/,
	'create a CREATEROLE user');
$node->issues_sql_like(
	[ 'createuser', '-s', 'regress_user3' ],
	qr/statement: CREATE ROLE regress_user3 SUPERUSER CREATEDB CREATEROLE INHERIT LOGIN;/,
	'create a superuser');
$node->issues_sql_like(
	[
		'createuser',    '-a',
		'regress_user1', '-a',
		'regress user2', 'regress user #4'
	],
	qr/statement: CREATE ROLE "regress user #4" NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN ADMIN regress_user1,"regress user2";/,
	'add a role as a member with admin option of the newly created role');
$node->issues_sql_like(
	[
		'createuser',      '-m',
		'regress_user3',   '-m',
		'regress user #4', 'REGRESS_USER5'
	],
	qr/statement: CREATE ROLE "REGRESS_USER5" NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN ROLE regress_user3,"regress user #4";/,
	'add a role as a member of the newly created role');
$node->issues_sql_like(
	[ 'createuser', '-v', '2029 12 31', 'regress_user6' ],
	qr/statement: CREATE ROLE regress_user6 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN VALID UNTIL \'2029 12 31\';/,
	'create a role with a password expiration date');
$node->issues_sql_like(
	[ 'createuser', '--bypassrls', 'regress_user7' ],
	qr/statement: CREATE ROLE regress_user7 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN BYPASSRLS;/,
	'create a BYPASSRLS role');
$node->issues_sql_like(
	[ 'createuser', '--no-bypassrls', 'regress_user8' ],
	qr/statement: CREATE ROLE regress_user8 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOBYPASSRLS;/,
	'create a role without BYPASSRLS');

$node->command_fails([ 'createuser', 'regress_user1' ],
	'fails if role already exists');

done_testing();
