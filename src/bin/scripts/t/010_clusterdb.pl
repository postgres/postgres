use strict;
use warnings;
use TestLib;
use Test::More tests => 13;

program_help_ok('clusterdb');
program_version_ok('clusterdb');
program_options_handling_ok('clusterdb');

my $tempdir = tempdir;
start_test_server $tempdir;

issues_sql_like(
	[ 'clusterdb', 'postgres' ],
	qr/statement: CLUSTER;/,
	'SQL CLUSTER run');

command_fails([ 'clusterdb', '-t', 'nonexistent', 'postgres' ],
	'fails with nonexistent table');

psql 'postgres',
'CREATE TABLE test1 (a int); CREATE INDEX test1x ON test1 (a); CLUSTER test1 USING test1x';
issues_sql_like(
	[ 'clusterdb', 'postgres', '-t', 'test1' ],
	qr/statement: CLUSTER test1;/,
	'cluster specific table');
