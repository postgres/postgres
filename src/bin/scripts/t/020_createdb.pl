
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 25;

program_help_ok('createdb');
program_version_ok('createdb');
program_options_handling_ok('createdb');

my $node = PostgreSQL::Test::Cluster->new('main');
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

# Check use of templates with shared dependencies copied from the template.
my ($ret, $stdout, $stderr) = $node->psql(
	'foobar2',
	'CREATE ROLE role_foobar;
CREATE TABLE tab_foobar (id int);
ALTER TABLE tab_foobar owner to role_foobar;
CREATE POLICY pol_foobar ON tab_foobar FOR ALL TO role_foobar;');
$node->issues_sql_like(
	[ 'createdb', '-l', 'C', '-T', 'foobar2', 'foobar3' ],
	qr/statement: CREATE DATABASE foobar3 TEMPLATE foobar2/,
	'create database with template');
($ret, $stdout, $stderr) = $node->psql(
	'foobar3',
	"SELECT pg_describe_object(classid, objid, objsubid) AS obj,
       pg_describe_object(refclassid, refobjid, 0) AS refobj
   FROM pg_shdepend s JOIN pg_database d ON (d.oid = s.dbid)
   WHERE d.datname = 'foobar3' ORDER BY obj;", on_error_die => 1);
chomp($stdout);
like(
	$stdout,
	qr/^policy pol_foobar on table tab_foobar\|role role_foobar
table tab_foobar\|role role_foobar$/,
	'shared dependencies copied over to target database');

# Check quote handling with incorrect option values.
$node->command_checks_all(
	[ 'createdb', '--encoding', "foo'; SELECT '1", 'foobar2' ],
	1,
	[qr/^$/],
	[qr/^createdb: error: "foo'; SELECT '1" is not a valid encoding name/s],
	'createdb with incorrect --encoding');
$node->command_checks_all(
	[ 'createdb', '--lc-collate', "foo'; SELECT '1", 'foobar2' ],
	1,
	[qr/^$/],
	[
		qr/^createdb: error: database creation failed: ERROR:  invalid locale name|^createdb: error: database creation failed: ERROR:  new collation \(foo'; SELECT '1\) is incompatible with the collation of the template database/s
	],
	'createdb with incorrect --lc-collate');
$node->command_checks_all(
	[ 'createdb', '--lc-ctype', "foo'; SELECT '1", 'foobar2' ],
	1,
	[qr/^$/],
	[
		qr/^createdb: error: database creation failed: ERROR:  invalid locale name|^createdb: error: database creation failed: ERROR:  new LC_CTYPE \(foo'; SELECT '1\) is incompatible with the LC_CTYPE of the template database/s
	],
	'createdb with incorrect --lc-ctype');
