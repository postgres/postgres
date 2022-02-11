
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$ENV{PGOPTIONS} = '--client-min-messages=WARNING';

$node->issues_sql_like(
	[ 'reindexdb', '-a' ],
	qr/statement: REINDEX.*statement: REINDEX/s,
	'reindex all databases');

done_testing();
