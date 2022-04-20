
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('vacuumdb');
program_version_ok('vacuumdb');
program_options_handling_ok('vacuumdb');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->issues_sql_like(
	[ 'vacuumdb', 'postgres' ],
	qr/statement: VACUUM.*;/,
	'SQL VACUUM run');
$node->issues_sql_like(
	[ 'vacuumdb', '-f', 'postgres' ],
	qr/statement: VACUUM \(FULL\).*;/,
	'vacuumdb -f');
$node->issues_sql_like(
	[ 'vacuumdb', '-F', 'postgres' ],
	qr/statement: VACUUM \(FREEZE\).*;/,
	'vacuumdb -F');
$node->issues_sql_like(
	[ 'vacuumdb', '-zj2', 'postgres' ],
	qr/statement: VACUUM \(ANALYZE\).*;/,
	'vacuumdb -zj2');
$node->issues_sql_like(
	[ 'vacuumdb', '-Z', 'postgres' ],
	qr/statement: ANALYZE.*;/,
	'vacuumdb -Z');
$node->issues_sql_like(
	[ 'vacuumdb', '--disable-page-skipping', 'postgres' ],
	qr/statement: VACUUM \(DISABLE_PAGE_SKIPPING\).*;/,
	'vacuumdb --disable-page-skipping');
$node->issues_sql_like(
	[ 'vacuumdb', '--skip-locked', 'postgres' ],
	qr/statement: VACUUM \(SKIP_LOCKED\).*;/,
	'vacuumdb --skip-locked');
$node->issues_sql_like(
	[ 'vacuumdb', '--skip-locked', '--analyze-only', 'postgres' ],
	qr/statement: ANALYZE \(SKIP_LOCKED\).*;/,
	'vacuumdb --skip-locked --analyze-only');
$node->command_fails(
	[ 'vacuumdb', '--analyze-only', '--disable-page-skipping', 'postgres' ],
	'--analyze-only and --disable-page-skipping specified together');
$node->issues_sql_like(
	[ 'vacuumdb', '--no-index-cleanup', 'postgres' ],
	qr/statement: VACUUM \(INDEX_CLEANUP FALSE\).*;/,
	'vacuumdb --no-index-cleanup');
$node->command_fails(
	[ 'vacuumdb', '--analyze-only', '--no-index-cleanup', 'postgres' ],
	'--analyze-only and --no-index-cleanup specified together');
$node->issues_sql_like(
	[ 'vacuumdb', '--no-truncate', 'postgres' ],
	qr/statement: VACUUM \(TRUNCATE FALSE\).*;/,
	'vacuumdb --no-truncate');
$node->command_fails(
	[ 'vacuumdb', '--analyze-only', '--no-truncate', 'postgres' ],
	'--analyze-only and --no-truncate specified together');
$node->issues_sql_like(
	[ 'vacuumdb', '--no-process-toast', 'postgres' ],
	qr/statement: VACUUM \(PROCESS_TOAST FALSE\).*;/,
	'vacuumdb --no-process-toast');
$node->command_fails(
	[ 'vacuumdb', '--analyze-only', '--no-process-toast', 'postgres' ],
	'--analyze-only and --no-process-toast specified together');
$node->issues_sql_like(
	[ 'vacuumdb', '-P', 2, 'postgres' ],
	qr/statement: VACUUM \(PARALLEL 2\).*;/,
	'vacuumdb -P 2');
$node->issues_sql_like(
	[ 'vacuumdb', '-P', 0, 'postgres' ],
	qr/statement: VACUUM \(PARALLEL 0\).*;/,
	'vacuumdb -P 0');
$node->command_ok([qw(vacuumdb -Z --table=pg_am dbname=template1)],
	'vacuumdb with connection string');

$node->command_fails(
	[qw(vacuumdb -Zt pg_am;ABORT postgres)],
	'trailing command in "-t", without COLUMNS');

# Unwanted; better if it failed.
$node->command_ok(
	[qw(vacuumdb -Zt pg_am(amname);ABORT postgres)],
	'trailing command in "-t", with COLUMNS');

$node->safe_psql(
	'postgres', q|
  CREATE TABLE "need""q(uot" (")x" text);
  CREATE TABLE vactable (a int, b int);
  CREATE VIEW vacview AS SELECT 1 as a;

  CREATE FUNCTION f0(int) RETURNS int LANGUAGE SQL AS 'SELECT $1 * $1';
  CREATE FUNCTION f1(int) RETURNS int LANGUAGE SQL AS 'SELECT f0($1)';
  CREATE TABLE funcidx (x int);
  INSERT INTO funcidx VALUES (0),(1),(2),(3);
  CREATE INDEX i0 ON funcidx ((f1(x)));
|);
$node->command_ok([qw|vacuumdb -Z --table="need""q(uot"(")x") postgres|],
	'column list');
$node->command_fails(
	[qw|vacuumdb -Zt funcidx postgres|],
	'unqualifed name via functional index');

$node->command_fails(
	[ 'vacuumdb', '--analyze', '--table', 'vactable(c)', 'postgres' ],
	'incorrect column name with ANALYZE');
$node->command_fails([ 'vacuumdb', '-P', -1, 'postgres' ],
	'negative parallel degree');
$node->issues_sql_like(
	[ 'vacuumdb', '--analyze', '--table', 'vactable(a, b)', 'postgres' ],
	qr/statement: VACUUM \(ANALYZE\) public.vactable\(a, b\);/,
	'vacuumdb --analyze with complete column list');
$node->issues_sql_like(
	[ 'vacuumdb', '--analyze-only', '--table', 'vactable(b)', 'postgres' ],
	qr/statement: ANALYZE public.vactable\(b\);/,
	'vacuumdb --analyze-only with partial column list');
$node->command_checks_all(
	[ 'vacuumdb', '--analyze', '--table', 'vacview', 'postgres' ],
	0,
	[qr/^.*vacuuming database "postgres"/],
	[qr/^WARNING.*cannot vacuum non-tables or special system tables/s],
	'vacuumdb with view');
$node->command_fails(
	[ 'vacuumdb', '--table', 'vactable', '--min-mxid-age', '0', 'postgres' ],
	'vacuumdb --min-mxid-age with incorrect value');
$node->command_fails(
	[ 'vacuumdb', '--table', 'vactable', '--min-xid-age', '0', 'postgres' ],
	'vacuumdb --min-xid-age with incorrect value');
$node->issues_sql_like(
	[
		'vacuumdb',   '--table', 'vactable', '--min-mxid-age',
		'2147483000', 'postgres'
	],
	qr/GREATEST.*relminmxid.*2147483000/,
	'vacuumdb --table --min-mxid-age');
$node->issues_sql_like(
	[ 'vacuumdb', '--min-xid-age', '2147483001', 'postgres' ],
	qr/GREATEST.*relfrozenxid.*2147483001/,
	'vacuumdb --table --min-xid-age');

done_testing();
