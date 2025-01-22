
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

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
	qr/statement: CREATE ROLE regress_user1 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS;/,
	'SQL CREATE USER run');
$node->issues_sql_like(
	[ 'createuser', '--no-login', 'regress_role1' ],
	qr/statement: CREATE ROLE regress_role1 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT NOLOGIN NOREPLICATION NOBYPASSRLS;/,
	'create a non-login role');
$node->issues_sql_like(
	[ 'createuser', '--createrole', 'regress user2' ],
	qr/statement: CREATE ROLE "regress user2" NOSUPERUSER NOCREATEDB CREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS;/,
	'create a CREATEROLE user');
$node->issues_sql_like(
	[ 'createuser', '--superuser', 'regress_user3' ],
	qr/statement: CREATE ROLE regress_user3 SUPERUSER CREATEDB CREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS;/,
	'create a superuser');
$node->issues_sql_like(
	[
		'createuser',
		'--with-admin' => 'regress_user1',
		'--with-admin' => 'regress user2',
		'regress user #4'
	],
	qr/statement: CREATE ROLE "regress user #4" NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS ADMIN regress_user1,"regress user2";/,
	'add a role as a member with admin option of the newly created role');
$node->issues_sql_like(
	[
		'createuser',
		'REGRESS_USER5',
		'--with-member' => 'regress_user3',
		'--with-member' => 'regress user #4'
	],
	qr/statement: CREATE ROLE "REGRESS_USER5" NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS ROLE regress_user3,"regress user #4";/,
	'add a role as a member of the newly created role');
$node->issues_sql_like(
	[ 'createuser', '--valid-until' => '2029 12 31', 'regress_user6' ],
	qr/statement: CREATE ROLE regress_user6 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS VALID UNTIL \'2029 12 31\';/,
	'create a role with a password expiration date');
$node->issues_sql_like(
	[ 'createuser', '--bypassrls', 'regress_user7' ],
	qr/statement: CREATE ROLE regress_user7 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION BYPASSRLS;/,
	'create a BYPASSRLS role');
$node->issues_sql_like(
	[ 'createuser', '--no-bypassrls', 'regress_user8' ],
	qr/statement: CREATE ROLE regress_user8 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS;/,
	'create a role without BYPASSRLS');
$node->issues_sql_like(
	[ 'createuser', '--with-admin' => 'regress_user1', 'regress_user9' ],
	qr/statement: CREATE ROLE regress_user9 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS ADMIN regress_user1;/,
	'--with-admin');
$node->issues_sql_like(
	[ 'createuser', '--with-member' => 'regress_user1', 'regress_user10' ],
	qr/statement: CREATE ROLE regress_user10 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS ROLE regress_user1;/,
	'--with-member');
$node->issues_sql_like(
	[ 'createuser', '--role' => 'regress_user1', 'regress_user11' ],
	qr/statement: CREATE ROLE regress_user11 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS IN ROLE regress_user1;/,
	'--role');
$node->issues_sql_like(
	[ 'createuser', 'regress_user12', '--member-of' => 'regress_user1' ],
	qr/statement: CREATE ROLE regress_user12 NOSUPERUSER NOCREATEDB NOCREATEROLE INHERIT LOGIN NOREPLICATION NOBYPASSRLS IN ROLE regress_user1;/,
	'--member-of');

$node->command_fails([ 'createuser', 'regress_user1' ],
	'fails if role already exists');
$node->command_fails(
	[
		'createuser',
		'regress_user1',
		'--with-member' => 'regress_user2',
		'regress_user3'
	],
	'fails for too many non-options');

done_testing();
