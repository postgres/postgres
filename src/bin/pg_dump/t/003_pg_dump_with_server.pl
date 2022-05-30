
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $node = PostgreSQL::Test::Cluster->new('main');
my $port = $node->port;

$node->init;
$node->start;

#########################################
# Verify that dumping foreign data includes only foreign tables of
# matching servers

$node->safe_psql('postgres', "CREATE FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s0 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s1 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s2 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE FOREIGN TABLE t0 (a int) SERVER s0");
$node->safe_psql('postgres', "CREATE FOREIGN TABLE t1 (a int) SERVER s1");
my ($cmd, $stdout, $stderr, $result);

command_fails_like(
	[ "pg_dump", '-p', $port, '--include-foreign-data=s0', 'postgres' ],
	qr/foreign-data wrapper \"dummy\" has no handler\r?\npg_dump: detail: Query was: .*t0/,
	"correctly fails to dump a foreign table from a dummy FDW");

command_ok(
	[ "pg_dump", '-p', $port, '-a', '--include-foreign-data=s2', 'postgres' ],
	"dump foreign server with no tables");

done_testing();
