
# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 2;

my $node = PostgresNode->new('main');
$node->init;
$node->start;

# clusterdb -a is not compatible with -d, hence enforce environment variable
# correctly.
$ENV{PGDATABASE} = 'postgres';

$node->issues_sql_like(
	[ 'clusterdb', '-a' ],
	qr/statement: CLUSTER.*statement: CLUSTER/s,
	'cluster all databases');
