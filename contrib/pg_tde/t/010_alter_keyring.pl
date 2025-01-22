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

# Restart the server
PGTDE::append_to_file("-- server restart");
$node->stop();

$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

$rt_value = $node->psql('postgres', "SELECT pg_tde_add_key_provider_file('file-vault-1','/tmp/pg_tde_keyring_1.per');", extra_params => ['-a']);
$rt_value = $node->psql('postgres', "SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault-1');", extra_params => ['-a']);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap_basic;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc (k) VALUES (5),(6);', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

#rename the keyring file to make it inaccessible
PGTDE::append_to_file("--moving keyring file--");
move("/tmp/pg_tde_keyring_1.per", "/tmp/pg_tde_keyring_2.per")
    or die "move failed: $!";

#restart the server and now the table should become inaccessible
PGTDE::append_to_file("-- server restart");
$node->stop();

$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

# this should fail
($cmdret, $stdout, $stderr) = $node->psql('postgres', "SELECT * FROM test_enc ORDER BY id ASC;", extra_params => ['-a']);
PGTDE::append_to_file($stderr);

#$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
#PGTDE::append_to_file($stdout);


#create a new key provider pointing to the moved keyring file
PGTDE::append_to_file("-- creating new key provider pointing to the moved file --");
$rt_value = $node->psql('postgres', "SELECT pg_tde_add_key_provider_file('file-vault-2','/tmp/pg_tde_keyring_2.per');", extra_params => ['-a']);
#update principal key to use the new key provider
PGTDE::append_to_file("-- Alter principal key to use new provider --");
$rt_value = $node->psql('postgres', "SELECT pg_tde_alter_principal_key_keyring('file-vault-2');", extra_params => ['-a']);

# this should work now
$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);


# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# DROP EXTENSION
$stdout = $node->safe_psql('postgres', 'DROP EXTENSION pg_tde CASCADE;', extra_params => ['-a']);
# Stop the server
$node->stop();

# compare the expected and out file
my $compare = PGTDE->compare_results();

# Test/check if expected and result/out file match. If Yes, test passes.
is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

# Done testing for this testcase file.
done_testing();
