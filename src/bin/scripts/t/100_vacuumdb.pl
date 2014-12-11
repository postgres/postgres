use strict;
use warnings;
use TestLib;
use Test::More tests => 18;

program_help_ok('vacuumdb');
program_version_ok('vacuumdb');
program_options_handling_ok('vacuumdb');

my $tempdir = tempdir;
start_test_server $tempdir;

issues_sql_like(
	[ 'vacuumdb', 'postgres' ],
	qr/statement: VACUUM;/,
	'SQL VACUUM run');
issues_sql_like(
	[ 'vacuumdb', '-f', 'postgres' ],
	qr/statement: VACUUM \(FULL\);/,
	'vacuumdb -f');
issues_sql_like(
	[ 'vacuumdb', '-F', 'postgres' ],
	qr/statement: VACUUM \(FREEZE\);/,
	'vacuumdb -F');
issues_sql_like(
	[ 'vacuumdb', '-z', 'postgres' ],
	qr/statement: VACUUM \(ANALYZE\);/,
	'vacuumdb -z');
issues_sql_like(
	[ 'vacuumdb', '-Z', 'postgres' ],
	qr/statement: ANALYZE;/,
	'vacuumdb -Z');
