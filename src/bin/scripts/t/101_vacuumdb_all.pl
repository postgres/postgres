use strict;
use warnings;

use PostgresNode;
use Test::More tests => 4;

my $node = get_new_node('main');
$node->init;
$node->start;

$node->issues_sql_like(
	[ 'vacuumdb', '-a' ],
	qr/statement: VACUUM.*statement: VACUUM/s,
	'vacuum all databases');

$node->safe_psql(
	'postgres', q(
	CREATE DATABASE regression_invalid;
	UPDATE pg_database SET datconnlimit = -2 WHERE datname = 'regression_invalid';
));
$node->command_ok([ 'vacuumdb', '-a' ],
  'invalid database not targeted by vacuumdb -a');

# Doesn't quite belong here, but don't want to waste time by creating an
# invalid database in 010_vacuumdb.pl as well.
$node->command_fails([ 'vacuumdb', '-d', 'regression_invalid'],
  'vacuumdb cannot target invalid database');
