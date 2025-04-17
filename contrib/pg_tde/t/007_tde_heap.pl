#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");

my $rt_value = $node->start;
ok($rt_value == 1, "Start Server");

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');

PGTDE::psql($node, 'postgres', 'CREATE TABLE test_enc(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap;');

PGTDE::append_to_result_file("-- server restart");
$node->stop();
$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

PGTDE::psql($node, 'postgres', "SELECT pg_tde_add_database_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');");
PGTDE::psql($node, 'postgres', "SELECT pg_tde_set_key_using_database_key_provider('test-db-key','file-vault');");

######################### test_enc1 (simple create table w tde_heap)

PGTDE::psql($node, 'postgres', 'CREATE TABLE test_enc1(id SERIAL,k VARCHAR(32),PRIMARY KEY (id)) USING tde_heap;');

PGTDE::psql($node, 'postgres', 'INSERT INTO test_enc1 (k) VALUES (\'foobar\'),(\'barfoo\');');

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc1 ORDER BY id ASC;');

######################### test_enc2 (create heap + alter to tde_heap)

PGTDE::psql($node, 'postgres', 'CREATE TABLE test_enc2(id SERIAL,k VARCHAR(32),PRIMARY KEY (id));');

PGTDE::psql($node, 'postgres', 'INSERT INTO test_enc2 (k) VALUES (\'foobar\'),(\'barfoo\');');

PGTDE::psql($node, 'postgres', 'ALTER TABLE test_enc2 SET ACCESS METHOD tde_heap;');

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc2 ORDER BY id ASC;');

######################### test_enc3 (default_table_access_method)

PGTDE::psql($node, 'postgres', 'SET default_table_access_method = "tde_heap"; CREATE TABLE test_enc3(id SERIAL,k VARCHAR(32),PRIMARY KEY (id));');

PGTDE::psql($node, 'postgres', 'INSERT INTO test_enc3 (k) VALUES (\'foobar\'),(\'barfoo\');');

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc3 ORDER BY id ASC;');

######################### test_enc4 (create heap + alter default)

PGTDE::psql($node, 'postgres', 'CREATE TABLE test_enc4(id SERIAL,k VARCHAR(32),PRIMARY KEY (id)) USING heap;');

PGTDE::psql($node, 'postgres', 'INSERT INTO test_enc4 (k) VALUES (\'foobar\'),(\'barfoo\');');

PGTDE::psql($node, 'postgres', 'SET default_table_access_method = "tde_heap"; ALTER TABLE test_enc4 SET ACCESS METHOD DEFAULT;');

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc4 ORDER BY id ASC;');

######################### test_enc5 (create tde_heap + truncate)

PGTDE::psql($node, 'postgres', 'CREATE TABLE test_enc5(id SERIAL,k VARCHAR(32),PRIMARY KEY (id)) USING tde_heap;');

PGTDE::psql($node, 'postgres', 'INSERT INTO test_enc5 (k) VALUES (\'foobar\'),(\'barfoo\');');

PGTDE::psql($node, 'postgres', 'CHECKPOINT;');

PGTDE::psql($node, 'postgres', 'TRUNCATE test_enc5;');

PGTDE::psql($node, 'postgres', 'INSERT INTO test_enc5 (k) VALUES (\'foobar\'),(\'barfoo\');');

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc5 ORDER BY id ASC;');

PGTDE::append_to_result_file("-- server restart");
$node->stop();
$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

sub verify_table
{
    PGTDE::append_to_result_file('###########################');

    my ($table) = @_;

    my $tablefile = $node->safe_psql('postgres', 'SHOW data_directory;');
    $tablefile .= '/';
    $tablefile .= $node->safe_psql('postgres', 'SELECT pg_relation_filepath(\''.$table.'\');');

    PGTDE::psql($node, 'postgres', 'SELECT * FROM ' . $table . ' ORDER BY id ASC;');

    my $strings = 'TABLEFILE FOR ' . $table . ' FOUND: ';
    $strings .= `(ls  $tablefile >/dev/null && echo -n yes) || echo -n no`;
    PGTDE::append_to_result_file($strings);

    $strings = 'CONTAINS FOO (should be empty): ';
    $strings .= `strings $tablefile | grep foo`;
    PGTDE::append_to_result_file($strings);
}

verify_table('test_enc1');
verify_table('test_enc2');
verify_table('test_enc3');
verify_table('test_enc4');
verify_table('test_enc5');

# Verify that we can't see the data in the file
my $tablefile2 = $node->safe_psql('postgres', 'SHOW data_directory;');
$tablefile2 .= '/';
$tablefile2 .= $node->safe_psql('postgres', 'SELECT pg_relation_filepath(\'test_enc2\');');

my  $strings = 'TABLEFILE2 FOUND: ';
$strings .= `(ls  $tablefile2 >/dev/null && echo yes) || echo no`;
PGTDE::append_to_result_file($strings);

$strings = 'CONTAINS FOO (should be empty): ';
$strings .= `strings $tablefile2 | grep foo`;
PGTDE::append_to_result_file($strings);

# Verify that we can't see the data in the file
my $tablefile3 = $node->safe_psql('postgres', 'SHOW data_directory;');
$tablefile3 .= '/';
$tablefile3 .= $node->safe_psql('postgres', 'SELECT pg_relation_filepath(\'test_enc3\');');

$strings = 'TABLEFILE3 FOUND: ';
$strings .= `(ls  $tablefile3 >/dev/null && echo yes) || echo no`;
PGTDE::append_to_result_file($strings);

$strings = 'CONTAINS FOO (should be empty): ';
$strings .= `strings $tablefile3 | grep foo`;
PGTDE::append_to_result_file($strings);

# Verify that we can't see the data in the file
my $tablefile4 = $node->safe_psql('postgres', 'SHOW data_directory;');
$tablefile4 .= '/';
$tablefile4 .= $node->safe_psql('postgres', 'SELECT pg_relation_filepath(\'test_enc4\');');

$strings = 'TABLEFILE4 FOUND: ';
$strings .= `(ls  $tablefile4 >/dev/null && echo yes) || echo no`;
PGTDE::append_to_result_file($strings);

$strings = 'CONTAINS FOO (should be empty): ';
$strings .= `strings $tablefile4 | grep foo`;
PGTDE::append_to_result_file($strings);

PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc1;');
PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc2;');
PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc3;');
PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc4;');
PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc5;');

PGTDE::psql($node, 'postgres', 'DROP EXTENSION pg_tde;');

$node->stop();

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

done_testing();
