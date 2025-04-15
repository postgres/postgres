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

open my $conf2, '>>', "/tmp/datafile-location";
print $conf2 "/tmp/keyring_data_file\n";
close $conf2;

my $rt_value = $node->start();
ok($rt_value == 1, "Start Server");

my ($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGTDE EXTENSION");
PGTDE::append_to_file($stdout);

$rt_value = $node->psql('postgres', "SELECT pg_tde_add_database_key_provider_file('file-provider', json_object( 'type' VALUE 'file', 'path' VALUE '/tmp/datafile-location' ));", extra_params => ['-a']);
$rt_value = $node->psql('postgres', "SELECT pg_tde_set_key_using_database_key_provider('test-db-key','file-provider');", extra_params => ['-a']);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc1(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc1 (k) VALUES (5),(6);', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc1 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc1 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc1;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP EXTENSION pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "DROP PGTDE EXTENSION");
PGTDE::append_to_file($stdout);

$node->stop();

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

done_testing();
