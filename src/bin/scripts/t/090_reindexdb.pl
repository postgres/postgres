use strict;
use warnings;
use TestLib;
use Test::More tests => 16;

program_help_ok('reindexdb');
program_version_ok('reindexdb');
program_options_handling_ok('reindexdb');

my $tempdir = tempdir;
start_test_server $tempdir;

$ENV{PGOPTIONS} = '--client-min-messages=WARNING';

issues_sql_like(
	[ 'reindexdb', 'postgres' ],
	qr/statement: REINDEX DATABASE postgres;/,
	'SQL REINDEX run');

psql 'postgres',
  'CREATE TABLE test1 (a int); CREATE INDEX test1x ON test1 (a);';
issues_sql_like(
	[ 'reindexdb', 'postgres', '-t', 'test1' ],
	qr/statement: REINDEX TABLE test1;/,
	'reindex specific table');
issues_sql_like(
	[ 'reindexdb', 'postgres', '-i', 'test1x' ],
	qr/statement: REINDEX INDEX test1x;/,
	'reindex specific index');

issues_sql_like(
	[ 'reindexdb', 'postgres', '-s' ],
	qr/statement: REINDEX SYSTEM postgres;/,
	'reindex system tables');
