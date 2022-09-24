
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

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

if ($ENV{with_icu} eq 'yes')
{
	# This fails because template0 uses libc provider and has no ICU
	# locale set.  It would succeed if template0 used the icu
	# provider.  XXX Maybe split into multiple tests?
	$node->command_fails(
		[
			'createdb', '-T', 'template0', '-E', 'UTF8',
			'--locale-provider=icu', 'foobar4'
		],
		'create database with ICU fails without ICU locale specified');

	$node->issues_sql_like(
		[
			'createdb',        '-T',
			'template0',       '-E', 'UTF8', '--locale-provider=icu',
			'--icu-locale=en', 'foobar5'
		],
		qr/statement: CREATE DATABASE foobar5 .* LOCALE_PROVIDER icu ICU_LOCALE 'en'/,
		'create database with ICU locale specified');

	$node->command_fails(
		[
			'createdb', '-T', 'template0', '-E', 'UTF8',
			'--locale-provider=icu',
			'--icu-locale=@colNumeric=lower', 'foobarX'
		],
		'fails for invalid ICU locale');

	$node->command_fails_like(
		[
			'createdb',             '-T',
			'template0',            '--locale-provider=icu',
			'--encoding=SQL_ASCII', 'foobarX'
		],
		qr/ERROR:  encoding "SQL_ASCII" is not supported with ICU provider/,
		'fails for encoding not supported by ICU');

	# additional node, which uses the icu provider
	my $node2 = PostgreSQL::Test::Cluster->new('icu');
	$node2->init(extra => ['--locale-provider=icu', '--icu-locale=en']);
	$node2->start;

	$node2->command_ok(
		[ 'createdb', '-T', 'template0', '--locale-provider=libc', 'foobar55' ],
		'create database with libc provider from template database with icu provider');

	$node2->command_ok(
		[ 'createdb', '-T', 'template0', '--icu-locale', 'en-US', 'foobar56' ],
		'create database with icu locale from template database with icu provider');
}
else
{
	$node->command_fails(
		[ 'createdb', '-T', 'template0', '--locale-provider=icu', 'foobar4' ],
		'create database with ICU fails since no ICU support');
}

$node->command_fails([ 'createdb', 'foobar1' ],
	'fails if database already exists');

$node->command_fails(
	[ 'createdb', '-T', 'template0', '--locale-provider=xyz', 'foobarX' ],
	'fails for invalid locale provider');

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

$node->command_checks_all(
	[ 'createdb', '--strategy', "foo", 'foobar2' ],
	1,
	[qr/^$/],
	[
		qr/^createdb: error: database creation failed: ERROR:  invalid create database strategy "foo"/s
	],
	'createdb with incorrect --strategy');

# Check database creation strategy
$node->issues_sql_like(
	[ 'createdb', '-T', 'foobar2', '-S', 'wal_log', 'foobar6' ],
	qr/statement: CREATE DATABASE foobar6 STRATEGY wal_log TEMPLATE foobar2/,
	'create database with WAL_LOG strategy');

$node->issues_sql_like(
	[ 'createdb', '-T', 'foobar2', '-S', 'file_copy', 'foobar7' ],
	qr/statement: CREATE DATABASE foobar7 STRATEGY file_copy TEMPLATE foobar2/,
	'create database with FILE_COPY strategy');

done_testing();
