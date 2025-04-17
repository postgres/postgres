#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

my $node = PGTDE->pgtde_init_pg();
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");

my $rt_value = $node->start;
ok($rt_value == 1, "Start Server");

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');
PGTDE::psql($node, 'postgres', "SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/unlogged_tables.per');");
PGTDE::psql($node, 'postgres', "SELECT pg_tde_set_key_using_database_key_provider('test-key', 'file-vault');");

PGTDE::psql($node, 'postgres', "CREATE UNLOGGED TABLE t (x int PRIMARY KEY) USING tde_heap;");

PGTDE::psql($node, 'postgres', "INSERT INTO t SELECT generate_series(1, 4);");

PGTDE::psql($node, 'postgres', "CHECKPOINT;");

PGTDE::append_to_result_file("-- kill -9");
$node->kill9();

PGTDE::append_to_result_file("-- server start");
$rt_value = $node->start;
ok($rt_value == 1, "Start Server");

PGTDE::psql($node, 'postgres', "TABLE t;");

PGTDE::psql($node, 'postgres', "INSERT INTO t SELECT generate_series(1, 4);");

$node->stop();

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0, "Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

done_testing();
