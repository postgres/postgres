
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Verify that work items work correctly

use strict;
use warnings;

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $node = PostgreSQL::Test::Cluster->new('tango');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum_naptime=1s');
$node->start;

$node->safe_psql('postgres', 'create extension pageinspect');

# Create a table with an autosummarizing BRIN index
$node->safe_psql(
	'postgres',
	'create table brin_wi (a int) with (fillfactor = 10);
	 create index brin_wi_idx on brin_wi using brin (a) with (pages_per_range=1, autosummarize=on);
	 '
);
my $count = $node->safe_psql('postgres',
	"select count(*) from brin_page_items(get_raw_page('brin_wi_idx', 2), 'brin_wi_idx'::regclass)"
);
is($count, '1', "initial index state is correct");

$node->safe_psql('postgres',
	'insert into brin_wi select * from generate_series(1, 100)');

$node->poll_query_until(
	'postgres',
	"select count(*) > 1 from brin_page_items(get_raw_page('brin_wi_idx', 2), 'brin_wi_idx'::regclass)",
	't');

$count = $node->safe_psql('postgres',
	"select count(*) > 1 from brin_page_items(get_raw_page('brin_wi_idx', 2), 'brin_wi_idx'::regclass)"
);
is($count, 't', "index got summarized");
$node->stop;

done_testing();
