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
close $conf;

unlink('/tmp/change_key_provider_1.per');
unlink('/tmp/change_key_provider_2.per');
unlink('/tmp/change_key_provider_3.per');
unlink('/tmp/change_key_provider_4.per');

# Start server
my $rt_value = $node->start;
ok($rt_value == 1, "Start Server");

# CREATE EXTENSION IF NOT EXISTS and change out file permissions
my ($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGTDE EXTENSION");
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_key_provider_file('file-vault', '/tmp/change_key_provider_1.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_list_all_key_providers();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_set_principal_key('test-key', 'file-vault');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc (id serial, k integer, PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc (k) VALUES (5), (6);', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_verify_principal_key();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_is_encrypted('test_enc');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Change provider and move file
PGTDE::append_to_file("-- mv /tmp/change_key_provider_1.per /tmp/change_key_provider_2.per");
move('/tmp/change_key_provider_1.per', '/tmp/change_key_provider_2.per');
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_change_key_provider_file('file-vault', '/tmp/change_key_provider_2.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_list_all_key_providers();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_verify_principal_key();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_is_encrypted('test_enc');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

# Verify
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_verify_principal_key();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_is_encrypted('test_enc');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Change provider and do not move file
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_change_key_provider_file('file-vault', '/tmp/change_key_provider_3.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_list_all_key_providers();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

(undef, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_verify_principal_key();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_is_encrypted('test_enc');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

# Verify
(undef, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_verify_principal_key();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
(undef, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_is_encrypted('test_enc');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
(undef, $stdout, $stderr) = $node->psql('postgres', 'SELECT * FROM test_enc ORDER BY id;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);

PGTDE::append_to_file("-- mv /tmp/change_key_provider_2.per /tmp/change_key_provider_3.per");
move('/tmp/change_key_provider_2.per', '/tmp/change_key_provider_3.per');

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

# Verify
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_verify_principal_key();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_is_encrypted('test_enc');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# DROP EXTENSION
(undef, $stdout, $stderr) = $node->psql('postgres', 'DROP EXTENSION pg_tde CASCADE;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);

# CREATE EXTENSION
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGTDE EXTENSION");
PGTDE::append_to_file($stdout);

# Change provider and generate a new principal key
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_key_provider_file('file-vault', '/tmp/change_key_provider_4.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->psql('postgres', "SELECT pg_tde_set_principal_key('test-key', 'file-vault');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc (id serial, k integer, PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc (k) VALUES (5), (6);', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_verify_principal_key();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_is_encrypted('test_enc');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_change_key_provider_file('file-vault', '/tmp/change_key_provider_3.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

# Verify
(undef, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_verify_principal_key();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
(undef, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_is_encrypted('test_enc');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
(undef, $stdout, $stderr) = $node->psql('postgres', 'SELECT * FROM test_enc ORDER BY id;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
(undef, $stdout, $stderr) = $node->psql('postgres', 'CREATE TABLE test_enc2 (id serial, k integer, PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_change_key_provider_file('file-vault', '/tmp/change_key_provider_4.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Verify
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_verify_principal_key();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_is_encrypted('test_enc');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# DROP EXTENSION
(undef, $stdout, $stderr) = $node->psql('postgres', 'DROP EXTENSION pg_tde CASCADE;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);

# Stop the server
$node->stop();

# compare the expected and out file
my $compare = PGTDE->compare_results();

# Test/check if expected and result/out file match. If Yes, test passes.
is($compare, 0, "Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

# Done testing for this testcase file.
done_testing();
