use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 14;

program_help_ok('dropdb');
program_version_ok('dropdb');
program_options_handling_ok('dropdb');

my $node = get_new_node('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE foobar1');
$node->issues_sql_like(
	[ 'dropdb', 'foobar1' ],
	qr/statement: DROP DATABASE foobar1/,
	'SQL DROP DATABASE run');

$node->safe_psql('postgres', 'CREATE DATABASE foobar2');
$node->issues_sql_like(
	[ 'dropdb', '--force', 'foobar2' ],
	qr/statement: DROP DATABASE foobar2 WITH \(FORCE\);/,
	'SQL DROP DATABASE (FORCE) run');

$node->command_fails([ 'dropdb', 'nonexistent' ],
	'fails with nonexistent database');

# check that invalid database can be dropped with dropdb
$node->safe_psql(
	'postgres', q(
	CREATE DATABASE regression_invalid;
	UPDATE pg_database SET datconnlimit = -2 WHERE datname = 'regression_invalid';
));
$node->command_ok([ 'dropdb', 'regression_invalid' ],
  'invalid database can be dropped');
