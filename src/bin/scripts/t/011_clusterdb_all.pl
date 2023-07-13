use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 4;

my $node = get_new_node('main');
$node->init;
$node->start;

# clusterdb -a is not compatible with -d, hence enforce environment variable
# correctly.
$ENV{PGDATABASE} = 'postgres';

$node->issues_sql_like(
	[ 'clusterdb', '-a' ],
	qr/statement: CLUSTER.*statement: CLUSTER/s,
	'cluster all databases');

$node->safe_psql(
	'postgres', q(
	CREATE DATABASE regression_invalid;
	UPDATE pg_database SET datconnlimit = -2 WHERE datname = 'regression_invalid';
));
$node->command_ok([ 'clusterdb', '-a' ],
  'invalid database not targeted by clusterdb -a');

# Doesn't quite belong here, but don't want to waste time by creating an
# invalid database in 010_clusterdb.pl as well.
$node->command_fails([ 'clusterdb', '-d', 'regression_invalid'],
  'clusterdb cannot target invalid database');
