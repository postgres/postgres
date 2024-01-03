
# Copyright (c) 2021-2023, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# clusterdb -a is not compatible with -d.  This relies on PGDATABASE to be
# set, something PostgreSQL::Test::Cluster does.
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
$node->command_fails_like([ 'clusterdb', '-d', 'regression_invalid'],
  qr/FATAL:  cannot connect to invalid database "regression_invalid"/,
  'clusterdb cannot target invalid database');

done_testing();
