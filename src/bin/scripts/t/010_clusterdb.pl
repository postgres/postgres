use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 14;

program_help_ok('clusterdb');
program_version_ok('clusterdb');
program_options_handling_ok('clusterdb');

my $node = get_new_node('main');
$node->init;
$node->start;

$node->issues_sql_like(
	['clusterdb'],
	qr/statement: CLUSTER;/,
	'SQL CLUSTER run');

$node->command_fails([ 'clusterdb', '-t', 'nonexistent' ],
	'fails with nonexistent table');

$node->safe_psql('postgres',
	'CREATE TABLE test1 (a int); CREATE INDEX test1x ON test1 (a); CLUSTER test1 USING test1x'
);
$node->issues_sql_like(
	[ 'clusterdb', '-t', 'test1' ],
	qr/statement: CLUSTER public\.test1;/,
	'cluster specific table');

$node->command_ok([qw(clusterdb --echo --verbose dbname=template1)],
	'clusterdb with connection string');
