
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->issues_sql_like(
	[ 'vacuumdb', '--analyze-in-stages', 'postgres' ],
	qr/statement:\ SET\ default_statistics_target=1;\ SET\ vacuum_cost_delay=0;
                   .*statement:\ ANALYZE
                   .*statement:\ SET\ default_statistics_target=10;\ RESET\ vacuum_cost_delay;
                   .*statement:\ ANALYZE
                   .*statement:\ RESET\ default_statistics_target;
                   .*statement:\ ANALYZE/sx,
	'analyze three times');

$node->issues_sql_like(
	[ 'vacuumdb', '--analyze-in-stages', '--all' ],
	qr/statement:\ SET\ default_statistics_target=1;\ SET\ vacuum_cost_delay=0;
                   .*statement:\ ANALYZE
                   .*statement:\ SET\ default_statistics_target=1;\ SET\ vacuum_cost_delay=0;
                   .*statement:\ ANALYZE
                   .*statement:\ SET\ default_statistics_target=10;\ RESET\ vacuum_cost_delay;
                   .*statement:\ ANALYZE
                   .*statement:\ SET\ default_statistics_target=10;\ RESET\ vacuum_cost_delay;
                   .*statement:\ ANALYZE
                   .*statement:\ RESET\ default_statistics_target;
                   .*statement:\ ANALYZE
                   .*statement:\ RESET\ default_statistics_target;
                   .*statement:\ ANALYZE/sx,
	'analyze more than one database in stages');

done_testing();
