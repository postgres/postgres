
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$ENV{PGOPTIONS} = '--client-min-messages=WARNING';

$node->safe_psql('postgres',
	'CREATE TABLE test1 (a int); CREATE INDEX test1x ON test1 (a);');
$node->safe_psql('template1',
	'CREATE TABLE test1 (a int); CREATE INDEX test1x ON test1 (a);');
$node->issues_sql_like(
	[ 'reindexdb', '--all' ],
	qr/statement: REINDEX.*statement: REINDEX/s,
	'reindex all databases');
$node->issues_sql_like(
	[ 'reindexdb', '--all', '--system' ],
	qr/statement: REINDEX SYSTEM postgres/s,
	'reindex system catalogs in all databases');
$node->issues_sql_like(
	[ 'reindexdb', '--all', '--schema' => 'public' ],
	qr/statement: REINDEX SCHEMA public/s,
	'reindex schema in all databases');
$node->issues_sql_like(
	[ 'reindexdb', '--all', '--index' => 'test1x' ],
	qr/statement: REINDEX INDEX public\.test1x/s,
	'reindex index in all databases');
$node->issues_sql_like(
	[ 'reindexdb', '--all', '--table' => 'test1' ],
	qr/statement: REINDEX TABLE public\.test1/s,
	'reindex table in all databases');

$node->safe_psql(
	'postgres', q(
	CREATE DATABASE regression_invalid;
	UPDATE pg_database SET datconnlimit = -2 WHERE datname = 'regression_invalid';
));
$node->command_ok([ 'reindexdb', '--all' ],
	'invalid database not targeted by reindexdb --all');

# Doesn't quite belong here, but don't want to waste time by creating an
# invalid database in 090_reindexdb.pl as well.
$node->command_fails_like(
	[ 'reindexdb', '--dbname' => 'regression_invalid' ],
	qr/FATAL:  cannot connect to invalid database "regression_invalid"/,
	'reindexdb cannot target invalid database');

done_testing();
