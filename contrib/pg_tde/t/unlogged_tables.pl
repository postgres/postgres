#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

unlink('/tmp/unlogged_tables.per');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");
$node->start;

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION pg_tde;');
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/unlogged_tables.per');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_create_key_using_database_key_provider('test-key', 'file-vault');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('test-key', 'file-vault');"
);

PGTDE::psql($node, 'postgres',
	"CREATE UNLOGGED TABLE t (x int PRIMARY KEY) USING tde_heap;");

PGTDE::psql($node, 'postgres', "INSERT INTO t SELECT generate_series(1, 4);");

PGTDE::psql($node, 'postgres', "CHECKPOINT;");

PGTDE::append_to_result_file("-- kill -9");
PGTDE::kill9_until_dead($node);

PGTDE::append_to_result_file("-- server start");
$node->start;

PGTDE::psql($node, 'postgres', "TABLE t;");

PGTDE::psql($node, 'postgres', "INSERT INTO t SELECT generate_series(1, 4);");

$node->stop;

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0,
	"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files."
);

done_testing();
