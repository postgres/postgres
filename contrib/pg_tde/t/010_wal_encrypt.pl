#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use File::Compare;
use File::Copy;
use Test::More;
use lib 't';
use pgtde;

# Get file name and CREATE out file name and dirs WHERE requried
PGTDE::setup_files_dir(basename($0));

# CREATE new PostgreSQL node and do initdb
my $node = PGTDE->pgtde_init_pg();
my $pgdata = $node->data_dir;

# UPDATE postgresql.conf to include/load pg_tde library
open my $conf, '>>', "$pgdata/postgresql.conf";
print $conf "shared_preload_libraries = 'pg_tde'\n";
# NOT testing that it can't start: the test framework doesn't have an easy way to do this
# print $conf "pg_tde.wal_encrypt = 1\n";
close $conf;

my $rt_value = $node->start;
ok($rt_value == 1, "Start Server");

my $stdout = $node->safe_psql('postgres', "CREATE EXTENSION IF NOT EXISTS pg_tde;", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_global_key_provider_file('file-keyring-010','/tmp/pg_tde_test_keyring010.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_set_server_principal_key('global-db-principal-key', 'file-keyring-010');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server, it should work with encryption now
PGTDE::append_to_file("-- server restart with wal encryption");
$node->stop();

open $conf, '>>', "$pgdata/postgresql.conf";
print $conf "pg_tde.wal_encrypt = 1\n";
close $conf;

$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

$stdout = $node->safe_psql('postgres', "SHOW pg_tde.wal_encrypt;", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_wal(id SERIAL,k INTEGER,PRIMARY KEY (id));', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'CHECKPOINT;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# TODO: add WAL content testing after the wal rework

# DROP EXTENSION
$stdout = $node->safe_psql('postgres', 'DROP EXTENSION pg_tde;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
# Stop the server
$node->stop();

# compare the expected and out file
my $compare = PGTDE->compare_results();

# Test/check if expected and result/out file match. If Yes, test passes.
is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

# Done testing for this testcase file.
done_testing();
