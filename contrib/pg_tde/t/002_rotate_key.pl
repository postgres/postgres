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

# Start server
my $rt_value = $node->start;
ok($rt_value == 1, "Start Server");

# CREATE EXTENSION IF NOT EXISTS and change out file permissions
my ($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGTDE EXTENSION");
PGTDE::append_to_file($stdout);


$rt_value = $node->psql('postgres', 'CREATE TABLE test_enc(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
ok($rt_value == 3, "Failing query");


# Restart the server
PGTDE::append_to_file("-- server restart");
$node->stop();

$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_key_provider_file('file-2','/tmp/pg_tde_test_keyring_2.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_global_key_provider_file('file-2','/tmp/pg_tde_test_keyring_2g.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_global_key_provider_file('file-3','/tmp/pg_tde_test_keyring_3.per');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_list_all_key_providers();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc (k) VALUES (5),(6);', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

#rotate key
$stdout = $node->psql('postgres', "SELECT pg_tde_set_principal_key('rotated-principal-key1');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

$stdout = $node->safe_psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_global_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);


#Again rotate key
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_set_principal_key('rotated-principal-key2','file-2');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

$stdout = $node->safe_psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_global_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

#Again rotate key
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_set_global_principal_key('rotated-principal-key', 'file-3', false);", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

$stdout = $node->safe_psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_global_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# TODO: add method to query current info
# And maybe debug tools to show what's in a file keyring?

#Again rotate key
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_set_global_principal_key('rotated-principal-keyX', 'file-2', false);", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

$stdout = $node->safe_psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_global_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'ALTER SYSTEM SET pg_tde.inherit_global_providers = OFF;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Things still work after a restart
# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

# But now can't be changed to another global provider
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_set_global_principal_key('rotated-principal-keyX2', 'file-2', false);", extra_params => ['-a']);
PGTDE::append_to_file($stderr);
$stdout = $node->safe_psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_global_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_set_principal_key('rotated-principal-key2','file-2');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
$stdout = $node->safe_psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT key_provider_id, key_provider_name, principal_key_name FROM pg_tde_global_principal_key_info();", extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);


# end

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'ALTER SYSTEM RESET pg_tde.inherit_global_providers;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$rt_value = $node->stop();
$rt_value = $node->start();

# DROP EXTENSION
($cmdret, $stdout, $stderr) = $node->psql('postgres', 'DROP EXTENSION pg_tde CASCADE;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);
PGTDE::append_to_file($stderr);
# Stop the server
$node->stop();

# compare the expected and out file
my $compare = PGTDE->compare_results();

# Test/check if expected and result/out file match. If Yes, test passes.
is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

# Done testing for this testcase file.
done_testing();
