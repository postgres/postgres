
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $dbname1 = 'regression_src';
my $dbname2 = 'regression_dest1';
my $dbname3 = 'regression_dest2';

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $backupdir = $node->backup_dir;

$node->run_log([ 'createdb', $dbname1 ]);
$node->run_log([ 'createdb', $dbname2 ]);
$node->run_log([ 'createdb', $dbname3 ]);

$node->safe_psql(
	$dbname1,
	qq{
create type digit as enum ('0', '1', '2', '3', '4', '5', '6', '7', '8', '9');

-- plain table with index
create table tplain (en digit, data int unique);
insert into tplain select (x%10)::text::digit, x from generate_series(1,1000) x;

-- non-troublesome hashed partitioning
create table ths (mod int, data int, unique(mod, data)) partition by hash(mod);
create table ths_p1 partition of ths for values with (modulus 3, remainder 0);
create table ths_p2 partition of ths for values with (modulus 3, remainder 1);
create table ths_p3 partition of ths for values with (modulus 3, remainder 2);
insert into ths select (x%10), x from generate_series(1,1000) x;

-- dangerous hashed partitioning
create table tht (en digit, data int, unique(en, data)) partition by hash(en);
create table tht_p1 partition of tht for values with (modulus 3, remainder 0);
create table tht_p2 partition of tht for values with (modulus 3, remainder 1);
create table tht_p3 partition of tht for values with (modulus 3, remainder 2);
insert into tht select (x%10)::text::digit, x from generate_series(1,1000) x;
	});

$node->command_ok(
	[
		'pg_dump', '-Fd', '--no-sync', '-j2', '-f', "$backupdir/dump1",
		$node->connstr($dbname1)
	],
	'parallel dump');

$node->command_ok(
	[
		'pg_restore', '-v',
		'-d', $node->connstr($dbname2),
		'-j3', "$backupdir/dump1"
	],
	'parallel restore');

$node->command_ok(
	[
		'pg_dump', '-Fd',
		'--no-sync', '-j2',
		'-f', "$backupdir/dump2",
		'--inserts', $node->connstr($dbname1)
	],
	'parallel dump as inserts');

$node->command_ok(
	[
		'pg_restore', '-v',
		'-d', $node->connstr($dbname3),
		'-j3', "$backupdir/dump2"
	],
	'parallel restore as inserts');

done_testing();
