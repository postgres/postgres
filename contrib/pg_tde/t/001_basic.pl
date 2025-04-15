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

my ($cmdret, $stdout, $stderr) = $node->psql('postgres', 'SELECT extname, extversion FROM pg_extension WHERE extname = \'pg_tde\';', extra_params => ['-a']);
ok($cmdret == 0, "SELECT PGTDE VERSION");
PGTDE::append_to_file($stdout);

$rt_value = $node->psql('postgres', 'CREATE TABLE test_enc(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
ok($rt_value == 3, "Failing query");

PGTDE::append_to_file("-- server restart");
$node->stop();
$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

$rt_value = $node->psql('postgres', "SELECT pg_tde_add_database_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');", extra_params => ['-a']);
$rt_value = $node->psql('postgres', "SELECT pg_tde_set_key_using_database_key_provider('test-db-key','file-vault');", extra_params => ['-a']);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc(id SERIAL,k VARCHAR(32),PRIMARY KEY (id)) USING tde_heap;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc (k) VALUES (\'foobar\'),(\'barfoo\');', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

PGTDE::append_to_file("-- server restart");
$node->stop();
$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Verify that we can't see the data in the file
my $tablefile = $node->safe_psql('postgres', 'SHOW data_directory;');
$tablefile .= '/';
$tablefile .= $node->safe_psql('postgres', 'SELECT pg_relation_filepath(\'test_enc\');');

my $strings = 'TABLEFILE FOUND: ';
$strings .= `(ls  $tablefile >/dev/null && echo yes) || echo no`;
PGTDE::append_to_file($strings);

$strings = 'CONTAINS FOO (should be empty): ';
$strings .= `strings $tablefile | grep foo`;
PGTDE::append_to_file($strings);

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP EXTENSION pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "DROP PGTDE EXTENSION");
PGTDE::append_to_file($stdout);

$node->stop();

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

done_testing();
