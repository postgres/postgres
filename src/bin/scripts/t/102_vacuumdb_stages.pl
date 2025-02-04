
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

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

$node->safe_psql('postgres',
	'CREATE TABLE regression_vacuumdb_test AS select generate_series(1, 10) a, generate_series(2, 11) b;');
$node->issues_sql_like(
	[ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_test', 'postgres' ],
	qr/statement:\ ANALYZE/sx,
	'--missing-only with missing stats');
$node->issues_sql_unlike(
	[ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_test', 'postgres' ],
	qr/statement:\ ANALYZE/sx,
	'--missing-only with no missing stats');

$node->safe_psql('postgres',
	'CREATE INDEX regression_vacuumdb_test_idx ON regression_vacuumdb_test (mod(a, 2));');
$node->issues_sql_like(
	[ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_test', 'postgres' ],
	qr/statement:\ ANALYZE/sx,
	'--missing-only with missing index expression stats');
$node->issues_sql_unlike(
	[ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_test', 'postgres' ],
	qr/statement:\ ANALYZE/sx,
	'--missing-only with no missing index expression stats');

$node->safe_psql('postgres',
	'CREATE STATISTICS regression_vacuumdb_test_stat ON a, b FROM regression_vacuumdb_test;');
$node->issues_sql_like(
	[ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_test', 'postgres' ],
	qr/statement:\ ANALYZE/sx,
	'--missing-only with missing extended stats');
$node->issues_sql_unlike(
	[ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_test', 'postgres' ],
	qr/statement:\ ANALYZE/sx,
	'--missing-only with no missing extended stats');

$node->safe_psql('postgres',
	"CREATE TABLE regression_vacuumdb_child (a INT) INHERITS (regression_vacuumdb_test);\n"
	. "INSERT INTO regression_vacuumdb_child VALUES (1, 2);\n"
	. "ANALYZE regression_vacuumdb_child;\n");
$node->issues_sql_like(
    [ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_test', 'postgres' ],
    qr/statement:\ ANALYZE/sx,
    '--missing-only with missing inherited stats');
$node->issues_sql_unlike(
    [ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_test', 'postgres' ],
    qr/statement:\ ANALYZE/sx,
    '--missing-only with no missing inherited stats');

$node->safe_psql('postgres',
	"CREATE TABLE regression_vacuumdb_parted (a INT) PARTITION BY LIST (a);\n"
	. "CREATE TABLE regression_vacuumdb_part1 PARTITION OF regression_vacuumdb_parted FOR VALUES IN (1);\n"
	. "INSERT INTO regression_vacuumdb_parted VALUES (1);\n"
	. "ANALYZE regression_vacuumdb_part1;\n");
$node->issues_sql_like(
    [ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_parted', 'postgres' ],
    qr/statement:\ ANALYZE/sx,
    '--missing-only with missing partition stats');
$node->issues_sql_unlike(
    [ 'vacuumdb', '--analyze-in-stages', '--missing-only', '-t', 'regression_vacuumdb_parted', 'postgres' ],
    qr/statement:\ ANALYZE/sx,
    '--missing-only with no missing partition stats');

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
