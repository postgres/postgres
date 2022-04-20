
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# clusterdb -a is not compatible with -d, hence enforce environment variable
# correctly.
$ENV{PGDATABASE} = 'postgres';

$node->issues_sql_like(
	[ 'clusterdb', '-a' ],
	qr/statement: CLUSTER.*statement: CLUSTER/s,
	'cluster all databases');

done_testing();
