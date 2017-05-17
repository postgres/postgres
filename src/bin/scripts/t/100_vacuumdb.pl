use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 19;

program_help_ok('vacuumdb');
program_version_ok('vacuumdb');
program_options_handling_ok('vacuumdb');

my $node = get_new_node('main');
$node->init;
$node->start;

$node->issues_sql_like(
	[ 'vacuumdb', 'postgres' ],
	qr/statement: VACUUM;/,
	'SQL VACUUM run');
$node->issues_sql_like(
	[ 'vacuumdb', '-f', 'postgres' ],
	qr/statement: VACUUM \(FULL\);/,
	'vacuumdb -f');
$node->issues_sql_like(
	[ 'vacuumdb', '-F', 'postgres' ],
	qr/statement: VACUUM \(FREEZE\);/,
	'vacuumdb -F');
$node->issues_sql_like(
	[ 'vacuumdb', '-z', 'postgres' ],
	qr/statement: VACUUM \(ANALYZE\);/,
	'vacuumdb -z');
$node->issues_sql_like(
	[ 'vacuumdb', '-Z', 'postgres' ],
	qr/statement: ANALYZE;/,
	'vacuumdb -Z');
$node->command_ok([qw(vacuumdb -Z --table=pg_am dbname=template1)],
	'vacuumdb with connection string');
