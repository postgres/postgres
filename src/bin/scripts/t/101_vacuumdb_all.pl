
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->issues_sql_like(
	[ 'vacuumdb', '-a' ],
	qr/statement: VACUUM.*statement: VACUUM/s,
	'vacuum all databases');
$node->command_fails(
	[ 'vacuumdb', '-a',  '-n', 'pg_catalog' ],
	'cannot vacuum specific schema(s) in all databases');

done_testing();
