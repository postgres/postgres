#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

my $node = PGTDE->pgtde_init_pg();
my $pgdata = $node->data_dir;

open my $conf, '>>', "$pgdata/postgresql.conf";
print $conf "shared_preload_libraries = 'pg_tde'\n";
close $conf;

my $rt_value = $node->start;
ok($rt_value == 1, "Start Server");

my ($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGTDE EXTENSION");
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/unlogged_tables.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_set_key_using_database_key_provider('test-key', 'file-vault');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "CREATE UNLOGGED TABLE t (x int PRIMARY KEY) USING tde_heap;", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "INSERT INTO t SELECT generate_series(1, 4);", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "CHECKPOINT;", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

PGTDE::append_to_file("-- kill -9");
$node->kill9();

PGTDE::append_to_file("-- server start");
$rt_value = $node->start;
ok($rt_value == 1, "Start Server");

$stdout = $node->safe_psql('postgres', "TABLE t;", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "INSERT INTO t SELECT generate_series(1, 4);", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$node->stop();

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0, "Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

done_testing();
