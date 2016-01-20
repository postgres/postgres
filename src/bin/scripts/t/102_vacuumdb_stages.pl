use strict;
use warnings;

use PostgresNode;
use Test::More tests => 4;

my $node = get_new_node('main');
$node->init;
$node->start;

$node->issues_sql_like(
	[ 'vacuumdb', '--analyze-in-stages', 'postgres' ],
qr/.*statement:\ SET\ default_statistics_target=1;\ SET\ vacuum_cost_delay=0;
                   .*statement:\ ANALYZE.*
                   .*statement:\ SET\ default_statistics_target=10;\ RESET\ vacuum_cost_delay;
                   .*statement:\ ANALYZE.*
                   .*statement:\ RESET\ default_statistics_target;
                   .*statement:\ ANALYZE/sx,
	'analyze three times');

$node->issues_sql_like(
	[ 'vacuumdb', '--analyze-in-stages', '--all' ],
qr/.*statement:\ SET\ default_statistics_target=1;\ SET\ vacuum_cost_delay=0;
                   .*statement:\ ANALYZE.*
                   .*statement:\ SET\ default_statistics_target=1;\ SET\ vacuum_cost_delay=0;
                   .*statement:\ ANALYZE.*
                   .*statement:\ SET\ default_statistics_target=10;\ RESET\ vacuum_cost_delay;
                   .*statement:\ ANALYZE.*
                   .*statement:\ SET\ default_statistics_target=10;\ RESET\ vacuum_cost_delay;
                   .*statement:\ ANALYZE.*
                   .*statement:\ RESET\ default_statistics_target;
                   .*statement:\ ANALYZE.*
                   .*statement:\ RESET\ default_statistics_target;
                   .*statement:\ ANALYZE/sx,
	'analyze more than one database in stages');
