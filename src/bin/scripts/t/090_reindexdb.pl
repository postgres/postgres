
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('reindexdb');
program_version_ok('reindexdb');
program_options_handling_ok('reindexdb');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$ENV{PGOPTIONS} = '--client-min-messages=WARNING';

# Create a tablespace for testing.
my $tbspace_path = $node->basedir . '/regress_reindex_tbspace';
mkdir $tbspace_path or die "cannot create directory $tbspace_path";
my $tbspace_name = 'reindex_tbspace';
$node->safe_psql('postgres',
	"CREATE TABLESPACE $tbspace_name LOCATION '$tbspace_path';");

$node->issues_sql_like(
	[ 'reindexdb', 'postgres' ],
	qr/statement: REINDEX DATABASE postgres;/,
	'SQL REINDEX run');

# Use text as data type to get a toast table.
$node->safe_psql('postgres',
	'CREATE TABLE test1 (a text); CREATE INDEX test1x ON test1 (a);');
# Collect toast table and index names of this relation, for later use.
my $toast_table = $node->safe_psql('postgres',
	"SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'test1'::regclass;"
);
my $toast_index = $node->safe_psql('postgres',
	"SELECT indexrelid::regclass FROM pg_index WHERE indrelid = '$toast_table'::regclass;"
);

$node->issues_sql_like(
	[ 'reindexdb', '-t', 'test1', 'postgres' ],
	qr/statement: REINDEX TABLE public\.test1;/,
	'reindex specific table');
$node->issues_sql_like(
	[ 'reindexdb', '-t', 'test1', '--tablespace', $tbspace_name, 'postgres' ],
	qr/statement: REINDEX \(TABLESPACE $tbspace_name\) TABLE public\.test1;/,
	'reindex specific table on tablespace');
$node->issues_sql_like(
	[ 'reindexdb', '-i', 'test1x', 'postgres' ],
	qr/statement: REINDEX INDEX public\.test1x;/,
	'reindex specific index');
$node->issues_sql_like(
	[ 'reindexdb', '-S', 'pg_catalog', 'postgres' ],
	qr/statement: REINDEX SCHEMA pg_catalog;/,
	'reindex specific schema');
$node->issues_sql_like(
	[ 'reindexdb', '-s', 'postgres' ],
	qr/statement: REINDEX SYSTEM postgres;/,
	'reindex system tables');
$node->issues_sql_like(
	[ 'reindexdb', '-v', '-t', 'test1', 'postgres' ],
	qr/statement: REINDEX \(VERBOSE\) TABLE public\.test1;/,
	'reindex with verbose output');
$node->issues_sql_like(
	[
		'reindexdb',    '-v',          '-t', 'test1',
		'--tablespace', $tbspace_name, 'postgres'
	],
	qr/statement: REINDEX \(VERBOSE, TABLESPACE $tbspace_name\) TABLE public\.test1;/,
	'reindex with verbose output and tablespace');

# the same with --concurrently
$node->issues_sql_like(
	[ 'reindexdb', '--concurrently', 'postgres' ],
	qr/statement: REINDEX DATABASE CONCURRENTLY postgres;/,
	'SQL REINDEX CONCURRENTLY run');

$node->issues_sql_like(
	[ 'reindexdb', '--concurrently', '-t', 'test1', 'postgres' ],
	qr/statement: REINDEX TABLE CONCURRENTLY public\.test1;/,
	'reindex specific table concurrently');
$node->issues_sql_like(
	[ 'reindexdb', '--concurrently', '-i', 'test1x', 'postgres' ],
	qr/statement: REINDEX INDEX CONCURRENTLY public\.test1x;/,
	'reindex specific index concurrently');
$node->issues_sql_like(
	[ 'reindexdb', '--concurrently', '-S', 'public', 'postgres' ],
	qr/statement: REINDEX SCHEMA CONCURRENTLY public;/,
	'reindex specific schema concurrently');
$node->command_fails([ 'reindexdb', '--concurrently', '-s', 'postgres' ],
	'reindex system tables concurrently');
$node->issues_sql_like(
	[ 'reindexdb', '--concurrently', '-v', '-t', 'test1', 'postgres' ],
	qr/statement: REINDEX \(VERBOSE\) TABLE CONCURRENTLY public\.test1;/,
	'reindex with verbose output concurrently');
$node->issues_sql_like(
	[
		'reindexdb', '--concurrently', '-v',          '-t',
		'test1',     '--tablespace',   $tbspace_name, 'postgres'
	],
	qr/statement: REINDEX \(VERBOSE, TABLESPACE $tbspace_name\) TABLE CONCURRENTLY public\.test1;/,
	'reindex concurrently with verbose output and tablespace');

# REINDEX TABLESPACE on toast indexes and tables fails.  This is not
# part of the main regression test suite as these have unpredictable
# names, and CONCURRENTLY cannot be used in transaction blocks, preventing
# the use of TRY/CATCH blocks in a custom function to filter error
# messages.
$node->command_checks_all(
	[
		'reindexdb',   '-t', $toast_table, '--tablespace',
		$tbspace_name, 'postgres'
	],
	1,
	[],
	[qr/cannot move system relation/],
	'reindex toast table with tablespace');
$node->command_checks_all(
	[
		'reindexdb',    '--concurrently', '-t', $toast_table,
		'--tablespace', $tbspace_name,    'postgres'
	],
	1,
	[],
	[qr/cannot move system relation/],
	'reindex toast table concurrently with tablespace');
$node->command_checks_all(
	[
		'reindexdb',   '-i', $toast_index, '--tablespace',
		$tbspace_name, 'postgres'
	],
	1,
	[],
	[qr/cannot move system relation/],
	'reindex toast index with tablespace');
$node->command_checks_all(
	[
		'reindexdb',    '--concurrently', '-i', $toast_index,
		'--tablespace', $tbspace_name,    'postgres'
	],
	1,
	[],
	[qr/cannot move system relation/],
	'reindex toast index concurrently with tablespace');

# connection strings
$node->command_ok([qw(reindexdb --echo --table=pg_am dbname=template1)],
	'reindexdb table with connection string');
$node->command_ok(
	[qw(reindexdb --echo dbname=template1)],
	'reindexdb database with connection string');
$node->command_ok(
	[qw(reindexdb --echo --system dbname=template1)],
	'reindexdb system with connection string');

# parallel processing
$node->safe_psql(
	'postgres', q|
	CREATE SCHEMA s1;
	CREATE TABLE s1.t1(id integer);
	CREATE INDEX ON s1.t1(id);
	CREATE SCHEMA s2;
	CREATE TABLE s2.t2(id integer);
	CREATE INDEX ON s2.t2(id);
	-- empty schema
	CREATE SCHEMA s3;
|);

$node->command_fails(
	[ 'reindexdb', '-j', '2', '-s', 'postgres' ],
	'parallel reindexdb cannot process system catalogs');
$node->command_fails(
	[ 'reindexdb', '-j', '2', '-i', 'i1', 'postgres' ],
	'parallel reindexdb cannot process indexes');
$node->issues_sql_like(
	[ 'reindexdb', '-j', '2', 'postgres' ],
	qr/statement:\ REINDEX SYSTEM postgres;
.*statement:\ REINDEX TABLE public\.test1/s,
	'parallel reindexdb for database issues REINDEX SYSTEM first');
# Note that the ordering of the commands is not stable, so the second
# command for s2.t2 is not checked after.
$node->issues_sql_like(
	[ 'reindexdb', '-j', '2', '-S', 's1', '-S', 's2', 'postgres' ],
	qr/statement:\ REINDEX TABLE s1.t1;/,
	'parallel reindexdb for schemas does a per-table REINDEX');
$node->command_ok(
	[ 'reindexdb', '-j', '2', '-S', 's3' ],
	'parallel reindexdb with empty schema');
$node->command_checks_all(
	[ 'reindexdb', '-j', '2', '--concurrently', '-d', 'postgres' ],
	0,
	[qr/^$/],
	[
		qr/^reindexdb: warning: cannot reindex system catalogs concurrently, skipping all/s
	],
	'parallel reindexdb for system with --concurrently skips catalogs');

done_testing();
