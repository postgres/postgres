use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 23;

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
	[ 'vacuumdb', '-zj2', 'postgres' ],
	qr/statement: VACUUM \(ANALYZE\) pg_catalog\./,
	'vacuumdb -zj2');
$node->issues_sql_like(
	[ 'vacuumdb', '-Z', 'postgres' ],
	qr/statement: ANALYZE;/,
	'vacuumdb -Z');
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
