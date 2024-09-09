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

# CREATE EXTENSION and change out file permissions
my ($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGTDE EXTENSION");
PGTDE::append_to_file($stdout);


($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE USER test_access;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE test_access USER");
PGTDE::append_to_file($stdout);

($cmdret, $stdout, $stderr) = $node->psql('postgres', 'grant all ON database postgres TO test_access;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE test_access USER");
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$node->stop();

$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

# TRY performing operations without permission
PGTDE::append_to_file("-- pg_tde_add_key_provider_file should throw access denied");
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stderr);

PGTDE::append_to_file("-- pg_tde_set_principal_key should also fail");
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stderr);

PGTDE::append_to_file("-- pg_tde_rotate_principal_key should give access denied error");
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_rotate_principal_key('rotated-principal-key','file-2');", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stderr);


# now give key management access to test_access user
PGTDE::append_to_file("-- grant key management access to test_access");
$stdout = $node->safe_psql('postgres', "select pg_tde_grant_key_management_to_role('test_access');", extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# TRY performing key operation with permission
$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($cmdret);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_add_key_provider_file('file-2','/tmp/pg_tde_test_keyring_2.per');", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT pg_tde_rotate_principal_key('rotated-principal-key','file-2');", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', "SELECT principal_key_name,key_provider_name,key_provider_id,principal_key_internal_name, principal_key_version from pg_tde_principal_key_info();", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($cmdret);


$stdout = $node->safe_psql('postgres', "SELECT pg_tde_list_all_key_providers();", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stdout);

# Now revoke the view access from test_access user
$stdout = $node->safe_psql('postgres', "select pg_tde_revoke_key_viewer_from_role('test_access');", extra_params => ['-a']);

# verify the view access is revoked

PGTDE::append_to_file("-- pg_tde_list_all_key_providers should also fail");
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT pg_tde_list_all_key_providers();", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stderr);

PGTDE::append_to_file("-- pg_tde_principal_key_info should also fail");
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT principal_key_name,key_provider_name,key_provider_id,principal_key_internal_name, principal_key_version from pg_tde_principal_key_info();", extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stderr);


$stdout = $node->safe_psql('postgres', 'CREATE SCHEMA test_access;', extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_access.test_enc1(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap_basic;', extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_access.test_enc1 (k) VALUES (5),(6);', extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_access.test_enc1 ORDER BY id ASC;', extra_params => ['-a', '-U', 'test_access']);
PGTDE::append_to_file($stdout);

# DROP EXTENSION
$stdout = $node->safe_psql('postgres', 'DROP EXTENSION pg_tde CASCADE;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Stop the server
$node->stop();

# compare the expected and out file
my $compare = PGTDE->compare_results();

# Test/check if expected and result/out file match. If Yes, test passes.
is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

# Done testing for this testcase file.
done_testing();
