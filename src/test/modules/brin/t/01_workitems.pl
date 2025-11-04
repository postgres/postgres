
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
# Another table with an index that requires a snapshot to run
$node->safe_psql(
	'postgres',
	'create table journal (d timestamp) with (fillfactor = 10);
	 create function packdate(d timestamp) returns text language plpgsql
	   as $$ begin return to_char(d, \'yyyymm\'); end; $$
	   returns null on null input immutable;
	 create index brin_packdate_idx on journal using brin (packdate(d))
	   with (autosummarize = on, pages_per_range = 1);
	 '
);

my $count = $node->safe_psql('postgres',
	"select count(*) from brin_page_items(get_raw_page('brin_wi_idx', 2), 'brin_wi_idx'::regclass)"
);
is($count, '1', "initial brin_wi_index index state is correct");
$count = $node->safe_psql('postgres',
	"select count(*) from brin_page_items(get_raw_page('brin_packdate_idx', 2), 'brin_packdate_idx'::regclass)"
);
is($count, '1', "initial brin_packdate_idx index state is correct");

$node->safe_psql('postgres',
	'insert into brin_wi select * from generate_series(1, 100)');
$node->safe_psql('postgres',
	"insert into journal select * from generate_series(timestamp '1976-08-01', '1976-10-28', '1 day')");

$node->poll_query_until(
	'postgres',
	"select count(*) > 1 from brin_page_items(get_raw_page('brin_wi_idx', 2), 'brin_wi_idx'::regclass)",
	't');

$count = $node->safe_psql('postgres',
	"select count(*) from brin_page_items(get_raw_page('brin_wi_idx', 2), 'brin_wi_idx'::regclass)
	 where not placeholder;"
);
cmp_ok($count, '>', '1', "$count brin_wi_idx ranges got summarized");

$node->poll_query_until(
	'postgres',
	"select count(*) > 1 from brin_page_items(get_raw_page('brin_packdate_idx', 2), 'brin_packdate_idx'::regclass)",
	't');

$count = $node->safe_psql('postgres',
	"select count(*) from brin_page_items(get_raw_page('brin_packdate_idx', 2), 'brin_packdate_idx'::regclass)
	 where not placeholder;"
);
cmp_ok($count, '>', '1', "$count brin_packdate_idx ranges got summarized");

$node->stop;

done_testing();
