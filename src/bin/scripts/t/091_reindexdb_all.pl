use strict;
use warnings;

use PostgresNode;
use Test::More tests => 2;

my $node = get_new_node('main');
$node->init;
$node->start;

$ENV{PGOPTIONS} = '--client-min-messages=WARNING';

$node->issues_sql_like(
	[ 'reindexdb', '-a' ],
	qr/statement: REINDEX.*statement: REINDEX/s,
	'reindex all databases');
